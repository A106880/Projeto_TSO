#!/bin/bash
# =============================================================================
# Benchmark Script - FUSE Deduplication vs Passthrough
# Executa testes FIO com syscounter/systracer e gera gráficos com gnuplot
# =============================================================================
# USO:
#   sudo ./benchmark.sh <pid_do_fuse>
#
# PRÉ-REQUISITOS:
#   - fio instalado (sudo apt install fio)
#   - gnuplot instalado (sudo apt install gnuplot)
#   - O FUSE filesystem já montado num mountpoint
#   - syscounter e systracer compilados
# =============================================================================

set -e

# ======================== CONFIGURAÇÃO ========================
FUSE_PID="${1:?Uso: sudo ./benchmark.sh <pid_do_fuse>}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESULTS_DIR="$SCRIPT_DIR/benchmark_results"
MOUNTPOINT="/mnt/fs"        # Alterar conforme o seu mountpoint
SYSCOUNTER="$SCRIPT_DIR/syscounter/syscounter"
SYSTRACER="$SCRIPT_DIR/systracer/systracer"

FIO_SIZE="64M"                      # Tamanho total por teste
FIO_BS="4k"                         # Tamanho do bloco (alinhado com o projeto)
FIO_RUNTIME="30"                    # Duração máxima em segundos
FIO_NUMJOBS="1"                     # Número de threads FIO

# ======================== PREPARAÇÃO ========================
mkdir -p "$RESULTS_DIR"

echo "============================================="
echo "  Benchmark FUSE Deduplication"
echo "============================================="
echo "PID do FUSE: $FUSE_PID"
echo "Mountpoint:  $MOUNTPOINT"
echo "Resultados:  $RESULTS_DIR"
echo "============================================="

if [ ! -d "$MOUNTPOINT" ]; then
    echo "ERRO: Mountpoint $MOUNTPOINT não existe. Altere a variável MOUNTPOINT no script."
    exit 1
fi

# ======================== FUNÇÕES AUXILIARES ========================

# Executa um teste FIO e captura métricas com syscounter
run_fio_test() {
    local TEST_NAME="$1"
    local FIO_EXTRA_ARGS="$2"
    local DEDUP_LEVEL="$3"
    local DO_CLEAN="${4:-yes}" # NOVO: Parâmetro para controlar a limpeza

    echo ""
    echo "--- Teste: $TEST_NAME (Dedup: ${DEDUP_LEVEL}%) ---"

    local FIO_OUTPUT="$RESULTS_DIR/${TEST_NAME}_fio.json"
    local SYSCOUNTER_OUTPUT="$RESULTS_DIR/${TEST_NAME}_syscounter.txt"
    local SYSTRACER_OUTPUT="$RESULTS_DIR/${TEST_NAME}_systracer.txt"

    # NOVO: Apenas limpa se DO_CLEAN for "yes"
    if [ "$DO_CLEAN" = "yes" ]; then
        # Limpar ficheiros antigos do mountpoint
        rm -f "$MOUNTPOINT"/testfile_* 2>/dev/null || true
    fi

    # Iniciar syscounter em background
    "$SYSCOUNTER" "$FUSE_PID" > "$SYSCOUNTER_OUTPUT" 2>&1 &
    local SC_PID=$!

    # Iniciar systracer em background
    "$SYSTRACER" "$FUSE_PID" > "$SYSTRACER_OUTPUT" 2>&1 &
    local ST_PID=$!

    sleep 1  # Dar tempo ao eBPF para se ligar

    # Executar FIO
    if [[ "$FIO_EXTRA_ARGS" == *.fio ]]; then
        local FIO_CMD="fio $FIO_EXTRA_ARGS --direct=1 --output-format=json --output=$FIO_OUTPUT"
    else
        local FIO_CMD="fio --name=$TEST_NAME \
            --directory=$MOUNTPOINT \
            --size=$FIO_SIZE \
            --bs=$FIO_BS \
            --direct=1 \
            --numjobs=$FIO_NUMJOBS \
            --runtime=$FIO_RUNTIME \
            --time_based=0 \
            --group_reporting \
            --output-format=json \
            --output=$FIO_OUTPUT \
            $FIO_EXTRA_ARGS"
    fi

    echo "  FIO: $FIO_CMD"
    eval $FIO_CMD 2>/dev/null || true

    sleep 2  # Dar tempo para eventos eBPF

    # Parar syscounter e systracer com SIGINT (para imprimir resultados)
    kill -INT "$SC_PID" 2>/dev/null || true
    kill -INT "$ST_PID" 2>/dev/null || true
    wait "$SC_PID" 2>/dev/null || true
    wait "$ST_PID" 2>/dev/null || true

    # Extrair métricas do FIO JSON
    if [ -f "$FIO_OUTPUT" ]; then
        local BW_READ=$(python3 -c "
import json, sys
try:
    d = json.load(open('$FIO_OUTPUT'))
    j = d['jobs'][0]
    print(j.get('read',{}).get('bw_mean',0) or j.get('read',{}).get('bw',0))
except: print(0)
" 2>/dev/null)
        local BW_WRITE=$(python3 -c "
import json, sys
try:
    d = json.load(open('$FIO_OUTPUT'))
    j = d['jobs'][0]
    print(j.get('write',{}).get('bw_mean',0) or j.get('write',{}).get('bw',0))
except: print(0)
" 2>/dev/null)
        local LAT_READ=$(python3 -c "
import json, sys
try:
    d = json.load(open('$FIO_OUTPUT'))
    j = d['jobs'][0]
    print(j.get('read',{}).get('lat_ns',{}).get('mean',0))
except: print(0)
" 2>/dev/null)
        local LAT_WRITE=$(python3 -c "
import json, sys
try:
    d = json.load(open('$FIO_OUTPUT'))
    j = d['jobs'][0]
    print(j.get('write',{}).get('lat_ns',{}).get('mean',0))
except: print(0)
" 2>/dev/null)
        local IOPS_READ=$(python3 -c "
import json, sys
try:
    d = json.load(open('$FIO_OUTPUT'))
    j = d['jobs'][0]
    print(j.get('read',{}).get('iops_mean',0) or j.get('read',{}).get('iops',0))
except: print(0)
" 2>/dev/null)
        local IOPS_WRITE=$(python3 -c "
import json, sys
try:
    d = json.load(open('$FIO_OUTPUT'))
    j = d['jobs'][0]
    print(j.get('write',{}).get('iops_mean',0) or j.get('write',{}).get('iops',0))
except: print(0)
" 2>/dev/null)

        echo "$TEST_NAME $DEDUP_LEVEL $BW_READ $BW_WRITE $LAT_READ $LAT_WRITE $IOPS_READ $IOPS_WRITE" \
            >> "$RESULTS_DIR/summary.dat"

        echo "  BW(R/W): ${BW_READ}/${BW_WRITE} KB/s | LAT(R/W): ${LAT_READ}/${LAT_WRITE} ns | IOPS(R/W): ${IOPS_READ}/${IOPS_WRITE}"
    fi

    echo "  Resultados guardados em $RESULTS_DIR/${TEST_NAME}_*"
}

# Gera um ficheiro FIO job com dados duplicados para simular deduplicação
create_dedup_fio_job() {
    local JOB_FILE="$1"
    local DEDUP_PERCENT="$2"
    local RW_TYPE="$3"

    cat > "$JOB_FILE" << EOF
[global]
directory=$MOUNTPOINT
bs=$FIO_BS
size=$FIO_SIZE
direct=1
numjobs=$FIO_NUMJOBS
group_reporting
dedupe_percentage=$DEDUP_PERCENT
buffer_compress_percentage=0
ioengine=psync
refill_buffers=1

[job]
rw=$RW_TYPE
filename=testfile_dedup${DEDUP_PERCENT}
EOF
}

# ======================== EXECUTAR TESTES ========================

# Limpar summary anterior
echo "# test_name dedup_pct bw_read bw_write lat_read lat_write iops_read iops_write" \
    > "$RESULTS_DIR/summary.dat"

echo ""
echo "========== FASE 1: Testes de Escrita Sequencial =========="

# Teste 1: Escrita sequencial - 0% deduplicação (dados únicos)
JOB="$RESULTS_DIR/write_dedup0.fio"
create_dedup_fio_job "$JOB" 0 "write"
run_fio_test "seq_write_dedup0" "$JOB" 0 "yes"

# Teste 2: Escrita sequencial - 50% deduplicação
JOB="$RESULTS_DIR/write_dedup50.fio"
create_dedup_fio_job "$JOB" 50 "write"
run_fio_test "seq_write_dedup50" "$JOB" 50 "yes"

# Teste 3: Escrita sequencial - 100% deduplicação (dados 100% repetidos)
JOB="$RESULTS_DIR/write_dedup100.fio"
create_dedup_fio_job "$JOB" 100 "write"
run_fio_test "seq_write_dedup100" "$JOB" 100 "yes"

echo ""
echo "========== FASE 2: Testes de Leitura Sequencial =========="

# Limpar ambiente antes de preparar o ficheiro de leitura
rm -f "$MOUNTPOINT"/testfile_* 2>/dev/null || true

# Escrever ficheiro para leitura
JOB="$RESULTS_DIR/precreate.fio"
create_dedup_fio_job "$JOB" 0 "write"
echo "  A pré-criar ficheiro para testes de leitura..."
fio "$JOB" --direct=1 --output=/dev/null 2>/dev/null || true

# Teste 4: Leitura sequencial
# NOVO: Passamos "no" no último argumento para NÃO apagar o testfile_dedup0
run_fio_test "seq_read" "--rw=read --filename=testfile_dedup0 --directory=$MOUNTPOINT" 0 "no"

echo ""
echo "========== FASE 3: Testes Multi-Thread =========="

# Teste 5: Escrita com 4 threads
JOB="$RESULTS_DIR/write_mt.fio"
cat > "$JOB" << EOF
[global]
directory=$MOUNTPOINT
bs=$FIO_BS
size=16M
direct=1
group_reporting
dedupe_percentage=50
ioengine=psync
refill_buffers=1

[job]
rw=write
numjobs=4
filename=testfile_mt
EOF
run_fio_test "mt_write_4threads" "$JOB" 50 "yes"

# ======================== GERAR GRÁFICOS ========================

echo ""
echo "========== A gerar gráficos... =========="

# Gráfico 1: Bandwidth de Escrita vs Nível de Deduplicação
cat > "$RESULTS_DIR/plot_bw.gnuplot" << 'GNUPLOT'
set terminal png size 800,500 enhanced font "Arial,12"
set output OUTPUT_FILE

set title "Bandwidth de Escrita vs Nível de Deduplicação"
set xlabel "Nível de Deduplicação (%)"
set ylabel "Bandwidth (KB/s)"
set grid
set style data linespoints
set pointsize 1.5
set key top left

# Filtrar apenas os testes seq_write
plot "< grep 'seq_write' DATA_FILE" using 2:4 title "Write BW" with linespoints lw 2 pt 7 lc rgb "#2196F3"
GNUPLOT
sed -i "s|OUTPUT_FILE|\"$RESULTS_DIR/graph_bw_write.png\"|" "$RESULTS_DIR/plot_bw.gnuplot"
sed -i "s|DATA_FILE|\"$RESULTS_DIR/summary.dat\"|" "$RESULTS_DIR/plot_bw.gnuplot"

# Gráfico 2: Latência de Escrita vs Nível de Deduplicação
cat > "$RESULTS_DIR/plot_lat.gnuplot" << 'GNUPLOT'
set terminal png size 800,500 enhanced font "Arial,12"
set output OUTPUT_FILE

set title "Latência de Escrita vs Nível de Deduplicação"
set xlabel "Nível de Deduplicação (%)"
set ylabel "Latência Média (ns)"
set grid
set style data linespoints
set pointsize 1.5

plot "< grep 'seq_write' DATA_FILE" using 2:6 title "Write Latency" with linespoints lw 2 pt 7 lc rgb "#F44336"
GNUPLOT
sed -i "s|OUTPUT_FILE|\"$RESULTS_DIR/graph_lat_write.png\"|" "$RESULTS_DIR/plot_lat.gnuplot"
sed -i "s|DATA_FILE|\"$RESULTS_DIR/summary.dat\"|" "$RESULTS_DIR/plot_lat.gnuplot"

# Gráfico 3: IOPS vs Nível de Deduplicação
cat > "$RESULTS_DIR/plot_iops.gnuplot" << 'GNUPLOT'
set terminal png size 800,500 enhanced font "Arial,12"
set output OUTPUT_FILE

set title "IOPS de Escrita vs Nível de Deduplicação"
set xlabel "Nível de Deduplicação (%)"
set ylabel "IOPS"
set grid
set style data linespoints
set pointsize 1.5

plot "< grep 'seq_write' DATA_FILE" using 2:8 title "Write IOPS" with linespoints lw 2 pt 7 lc rgb "#4CAF50"
GNUPLOT
sed -i "s|OUTPUT_FILE|\"$RESULTS_DIR/graph_iops_write.png\"|" "$RESULTS_DIR/plot_iops.gnuplot"
sed -i "s|DATA_FILE|\"$RESULTS_DIR/summary.dat\"|" "$RESULTS_DIR/plot_iops.gnuplot"

# Gráfico 4: Comparação geral (barras)
cat > "$RESULTS_DIR/plot_comparison.gnuplot" << 'GNUPLOT'
set terminal png size 1000,600 enhanced font "Arial,12"
set output OUTPUT_FILE

set title "Comparação: BW Escrita vs Leitura por Teste"
set xlabel "Teste"
set ylabel "Bandwidth (KB/s)"
set grid ytics
set style data histogram
set style histogram clustered gap 1
set style fill solid 0.8 border -1
set boxwidth 0.9
set xtics rotate by -30

plot DATA_FILE using 3:xtic(1) title "Read BW" lc rgb "#2196F3", \
     "" using 4 title "Write BW" lc rgb "#F44336"
GNUPLOT
sed -i "s|OUTPUT_FILE|\"$RESULTS_DIR/graph_comparison.png\"|" "$RESULTS_DIR/plot_comparison.gnuplot"
sed -i "s|DATA_FILE|\"$RESULTS_DIR/summary.dat\"|" "$RESULTS_DIR/plot_comparison.gnuplot"

# Executar gnuplot
for PLOT_FILE in "$RESULTS_DIR"/plot_*.gnuplot; do
    gnuplot "$PLOT_FILE" 2>/dev/null && echo "  Gerado: $(basename "${PLOT_FILE%.gnuplot}.png")" || \
        echo "  AVISO: Falha ao gerar $(basename "$PLOT_FILE") (gnuplot instalado?)"
done

# ======================== RELATÓRIO RESUMO ========================

echo ""
echo "========== RESUMO FINAL =========="
echo ""
printf "%-25s %-10s %-12s %-12s %-14s %-14s %-10s %-10s\n" \
    "TESTE" "DEDUP%" "BW_RD(KB/s)" "BW_WR(KB/s)" "LAT_RD(ns)" "LAT_WR(ns)" "IOPS_RD" "IOPS_WR"
printf '%0.s-' {1..105}; echo ""

tail -n +2 "$RESULTS_DIR/summary.dat" | while read -r name dedup bwr bww latr latw iopsr iopsw; do
    printf "%-25s %-10s %-12s %-12s %-14s %-14s %-10s %-10s\n" \
        "$name" "$dedup" "$bwr" "$bww" "$latr" "$latw" "$iopsr" "$iopsw"
done

echo ""
echo "============================================="
echo "  Benchmark completo!"
echo "  Resultados em: $RESULTS_DIR/"
echo "  Gráficos: $RESULTS_DIR/graph_*.png"
echo "  Dados syscounter: $RESULTS_DIR/*_syscounter.txt"
echo "  Dados systracer: $RESULTS_DIR/*_systracer.txt"
echo "============================================="