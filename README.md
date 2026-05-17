# Deduplication FUSE Filesystem & Benchmarking Suite

Este projeto implementa um sistema de ficheiros FUSE com suporte para deduplicação e inclui uma suite completa de benchmarks e ferramentas de análise.

## 1. Compilação

Para compilar o sistema de ficheiros (FUSE):

```bash
# Compilar a versão com Deduplicação (usando o Makefile em codededup/)
cd codededup && make clean && make

# O binário gerado chama-se 'passthrough' (dentro de codededup/)
```

---

## 2. Execução Manual do FUSE

Se quiseres correr a implementação manualmente sem usar os scripts de benchmark:

```bash
# Criar diretórios necessários
mkdir -p /mnt/fs
mkdir -p /backend

# Executar o FUSE (em foreground para ver logs)
sudo ./passthrough /mnt/fs -omodules="subdir,subdir=/backend" -oallow_other -f
```

Para desmontar:
```bash
sudo fusermount3 -u /mnt/fs
```

---

## 3. Execução de Benchmarks

Os benchmarks estão organizados por categorias dentro da pasta `script-teste/`.

### Correr um teste individual
Podes correr qualquer script `.sh` diretamente:
```bash
sudo ./script-teste/write50dedup/test_write50_mt_dedup.sh
```

### Correr todos os testes sequencialmente
```bash
sudo ./script-teste/run_all.sh
```

### Observabilidade e Toggles (Variáveis de Ambiente)
Por defeito, os testes correm "limpos" (apenas o FIO). Podes ativar ferramentas de monitorização usando variáveis de ambiente:

*   `USE_PIDSTAT=true`: Ativa monitorização de CPU/RAM (pidstat).
*   `USE_PERF=true`: Ativa profiling com `perf record`.
*   `USE_SYSCOUNTER=true`: Ativa contagem de syscalls (eBPF).
*   `USE_SYSTRACER=true`: Ativa tracing detalhado (eBPF).
*   `USE_ALL=true`: Ativa todas as ferramentas suportadas pelo script.

**Exemplos:**
```bash
# Correr com monitorização de memória
sudo USE_PIDSTAT=true ./script-teste/aging/test_aging_dedup.sh

# Correr tudo com observabilidade total
sudo USE_ALL=true ./script-teste/run_all.sh
```

---

## 4. Análise de Resultados

Após correres os testes, os dados brutos (JSON, TXT, DATA) ficam guardados na pasta `benchmark_results/`.

### Análise Automática (Global)
Para gerar resumos legíveis de todos os testes realizados:
```bash
python3 benchmark_analyse/full_analyser.py
```
Este comando cria ficheiros `resume_<categoria>.txt` na raiz do projeto com o processamento de todos os dados capturados.

### Analisadores Individuais
Também podes analisar ficheiros específicos:
```bash
# Analisar resultados do FIO
python3 benchmark_analyse/fio_analyser.py benchmark_results/categoria/teste_fio.json

# Analisar consumo de recursos
python3 benchmark_analyse/pidstat_analyser.py benchmark_results/categoria/teste_pidstat.txt

# Analisar Syscalls
python3 benchmark_analyse/syscounter_analyser.py benchmark_results/categoria/teste_syscounter.txt

# Analisar Tracer detalhado
python3 benchmark_analyse/systracer_analyser.py benchmark_results/categoria/teste_systracer.txt
```

---

## Estrutura do Projeto
*   `codededup/`: Implementação do FUSE com Deduplicação.
*   `codebase/`: Código base (skeleton) do FUSE.
*   `script-teste/`: Scripts de automação de testes (Bash).
*   `benchmark_analyse/`: Ferramentas de processamento de dados (Python).
*   `benchmark_results/`: Pasta onde os resultados dos testes são depositados.
