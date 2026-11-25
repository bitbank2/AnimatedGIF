# AnimatedGIF LowMem — Engineering Proposal

> Proposta experimental para reduzir RAM, melhorar a robustez e limitar a latência de decodificação GIF em microcontroladores, preservando a API existente.

## 1. Contexto

O [AnimatedGIF](https://github.com/bitbank2/AnimatedGIF) já oferece decodificação por linha, modo Turbo, buffers externos e rotinas específicas para algumas arquiteturas. A proposta é adicionar um caminho opcional de baixo consumo de memória, especialmente útil no ESP8266 (LX106), sem substituir o caminho otimizado existente.

## 2. Baseline de memória

Valores derivados de `src/AnimatedGIF.h` no commit de referência `17a6a2eda4385c14f39ee51adfc89ab721150527`, considerando `MAX_CODE_SIZE=12` e `MAX_WIDTH=480` (em `__LINUX__`, o upstream define `MAX_WIDTH=2048`):

| Buffer | Bytes |
|---|---:|
| `ucFileBuf` | 4096 |
| `usGIFTable` | 8192 |
| `ucGIFPixels` | 8192 |
| `ucLZW` | 1530 |
| `pPalette` | 768 |
| `pLocalPalette` | 768 |
| `ucLineBuf` | 495 |
| **Subtotal** | **24041 (23,48 KiB)** |

O subtotal não representa `sizeof(GIFIMAGE)`: exclui metadados, alinhamento, stack e buffers opcionais. Confirmar a baseline por compilação em cada plataforma (ver seção 9).

> **Nota de validação estática (acoplamento com Turbo):** No código upstream (`src/AnimatedGIF.h` e `src/gif.inl`), `DecodeLZWTurbo()` assume que `ucLZW`, `usGIFTable`, `ucGIFPixels` e `ucLineBuf` são contíguos na struct `GIFIMAGE` e utiliza `LZW_BUF_SIZE_TURBO` escrevendo além do fim lógico de `ucLZW` sobre essas tabelas. Portanto, perfis LowMem que alterem `usGIFTable`, `ucGIFPixels` ou `LZW_BUF_SIZE` precisam desabilitar o caminho Turbo em tempo de compilação (`GIF_DISABLE_TURBO`) ou protegê-lo com `#error`, evitando corrupção silenciosa de memória.

## 3. Dicionário LZW compacto

### Proposta

- Manter suporte aos 4096 códigos do GIF (12 bits).
- Substituir as duas tabelas de símbolos (`PIXEL_FIRST` + `PIXEL_LAST`) por uma tabela única de sufixos (`4096 B`) e reconstruir o primeiro símbolo durante a expansão da cadeia (o último byte gravado na pilha reversa já é o primeiro símbolo, sem travessia adicional).
- Avaliar prefixos de 16 bits inicialmente (`8192 B + 4096 B`); depois, testar prefixos compactados de 12 bits (`6144 B + 4096 B`, 2 entradas a cada 3 bytes).
- Preservar o caminho Turbo e a implementação padrão; introduzir a variante LowMem por opção de compilação.

| Representação | Dicionário | Economia teórica |
|---|---:|---:|
| Atual (`PIXEL_FIRST` + `PIXEL_LAST` + `uint16_t` prefix) | 16384 B | — |
| Prefixo de 16 bits + sufixo de 8 bits | 12288 B | 4096 B |
| Prefixo de 12 bits + sufixo de 8 bits | 10240 B | 6144 B |

Com os demais buffers inalterados, os subtotais projetados são 19945 B e 17897 B, respectivamente. A meta de 12–16 KiB requer otimizações adicionais; não decorre apenas da compactação do dicionário.

> **Nota de validação estática (meta de 12–16 KiB mantendo 12 bits completos):** Além da compactação do dicionário, três reduções ortogonais viabilizam atingir 12–16 KiB preservando 100% de compatibilidade com GIFs de 12 bits:
> 1. Reduzir `ucFileBuf` (ex.: `4096 B -> 768 B`, economia de `3328 B`), mantendo um fallback em múltiplos passos caso uma cadeia LZW válida exceda `FILE_BUF_SIZE` (ex.: imagens altas de até 32767 px).
> 2. Reduzir `ucLZW` (ex.: `1530 B -> 512 B` ou `768 B`, economia de `762–1018 B`) com leitor de bits por acumulador byte-a-byte.
> 3. Opção `GIF_RGB565_ONLY` reduzindo `pPalette` e `pLocalPalette` de `768 B` para `512 B` cada (`-512 B`).

### Invariantes

- Tratar corretamente Clear Code, EOI, `KwKwK` e transições de largura de código.
- Aceitar dicionários cheios com Clear Code diferido, sem sobrescrever entradas.
- Limitar a expansão ao tamanho da pilha (ou executar emissão fatiada segura) e validar cada prefixo.
- Comparar custo de reconstrução e de acesso a prefixos compactados com o caminho atual.

## 4. Lazy dictionary reset

Em `src/gif.inl`, `DecodeLZW()` reinicializa parte da tabela com `memset(&giftabs[cc], LINK_UNUSED, ...)` a cada Clear Code. Investigar a eliminação dessa limpeza usando `nextcode` como fronteira de validade das entradas.

O reset deve restaurar a largura inicial, a máscara, `nextcode = cc + 2`, `nextlim` e o estado do código anterior. Entradas antigas nunca podem ser acessadas antes de sua redefinição: toda entrada válida inserida em `nextcode` recebe `prefix = oldcode < nextcode`, garantindo cadeia estritamente decrescente até `code < cc` (símbolo raiz). Com isso, `LINK_UNUSED` e `LINK_END` deixam de ser necessários, permitindo inclusive armazenar prefixos em 12 bits puros (`0..4095`).

> **Nota de validação estática (borda Clear Code -> EOI):** Tanto no upstream quanto em implementações derivadas simples, um `EOI` imediatamente após `Clear Code` pode ser tratado incorretamente como código de pixel ou erro. O reset preguiçoso precisa tratar explicitamente múltiplos Clear Codes consecutivos e `EOI` logo após Clear Code.

**Hipótese:** menos escritas de RAM e menor custo de reset. Medir em GIFs com diferentes frequências de Clear Code.

## 5. Robustez e segurança de memória

### Pontos de investigação

- `GIFMakePels()`: o limite `LINK_UNUSED` (5911) é superior à capacidade física do dicionário padrão (4096). Verificar cadeias inválidas e validar limites físicos e lógicos antes de acessar tabelas. (Reproduzido empiricamente no upstream `17a6a2ed` com ASan: `code` entre 4096 e 5910 causa `global-buffer-overflow` em `giftabs`/`gifpels`; além disso, `allocFrameBuf()` usando `w*(h+3)` com `COOKED` + `disposal=2` disparou `heap-buffer-overflow` em `gif.inl:1620`.)
- `GIFGetMoreData()`: verificar os retornos de `pfnRead`, inclusive leituras parciais (fazendo loop enquanto retornar `> 0`), EOF (`0`) e erros (`<= 0`); nunca contabilizar bytes não lidos em `iLZWSize`. (No upstream `17a6a2ed`, callback retornando 1 byte por chamada — `CB_1BYTE` — falhou com `err=6`.)
- Leitor de bits (`GET_CODE` / `GET_CODE_TURBO`): garantir que exista informação suficiente para cada código e para cada leitura de palavra, inclusive nas bordas dos buffers, eliminando leituras de 4/8 bytes além do fim de `ucLZW` e acessos `uint64_t`/`uint16_t` desalinhados detectados por UBSan em `17a6a2ed`.
- Propagar falhas de expansão em vez de retornar silenciosamente de `GIFMakePels()`.
- Validar dimensões, offsets e operações de tamanho de alocação (`allocFrameBuf`, `allocTurboBuf`) com aritmética verificada.
- Fazer `nextcode` saturar quando o dicionário estiver cheio (`1 << MAX_CODE_SIZE`); testar fluxos com Clear Code diferido.
- **Achados adicionais reproduzidos no upstream `17a6a2ed`:**
  - `GIFParseInfo()` (`gif.inl:318`) rejeitava GIFs válidos menores que ~267 bytes com 4+ cores (`edge_small_1x1_2bit.gif` falhou com `err=8`);
  - `cGIFBits[b[0]]` (`gif.inl:525`) indexava fora dos limites com valor 255 sob UBSan;
  - 10 arquivos sintéticos malformados de LZW/paleta retornavam `err=0` silenciosamente;
  - `GIF_getInfo()` e `getComment()` executavam leituras sem verificação completa de limites e `DecodeLZW()` não drenava sub-blocos restantes ao encerrar por `iYCount == 0` antes do `EOI`.

### Testes

Executar corpus de GIFs válidos e malformados, testes de regressão com comparação de hashes pixel-a-pixel contra o upstream, fuzzing no host com ASan/UBSan e testes de leitura parcial (1 byte por chamada) via callbacks. Separar correções de robustez das mudanças de desempenho em commits/PRs independentes.

## 6. Decodificação cooperativa

A API atual processa o frame sincronamente. Uma API opcional preserva o estado do decodificador entre chamadas e impõe um orçamento aproximado de tempo (`decodeStep`) ou de linhas (`decodeLines`):

```cpp
gif.beginFrame(pUser);
while (!gif.frameComplete()) {
    auto status = gif.decodeStep(2000); // orçamento aproximado em microssegundos
    if (status == GIF_DECODE_ERR) break;
    yield();
}
```

Preservar cursor de bits, acumulador, dicionário, expansão pendente, posição de linha e estado de composição. Definir estados de retorno para progresso (`GIF_DECODE_MORE`), frame concluído (`GIF_DECODE_FRAME_DONE`), fim do GIF (`GIF_DECODE_END`) e erro (`GIF_DECODE_ERR`). O orçamento não é garantia de tempo real: callbacks de I/O e desenho podem bloquear. Manter `playFrame()` como interface compatível.

**Benefício esperado:** menor latência de serviço para Wi-Fi, HTTP e outras tarefas, particularmente no ESP8266.

## 7. Buffer de entrada LZW

`GIFGetMoreData()` desloca manualmente os bytes não consumidos para o início do buffer.

1. Comparar o loop atual com `memmove()` (correto para regiões sobrepostas).
2. Avaliar um leitor de bits com acumulador de 32 bits (`uint32_t` alimentado byte a byte) que elimina compactações intermediárias enquanto há bytes no buffer, evita `INTELLONG` desalinhado e elimina over-reads.
3. Testar códigos cruzando limites de sub-blocos GIF de 1 a 255 bytes e do buffer.
4. Avaliar redução de `LZW_BUF_SIZE` (ex.: `264 B`, `512 B`, `768 B`, `1530 B`), considerando frequência de callbacks e custo de I/O.

Não assumir que uma estratégia é necessariamente mais rápida; medir antes de adotar.

## 8. Perfis de compilação

| Perfil experimental | Finalidade |
|---|---|
| `GIF_PROFILE_FAST` | Caminho padrão otimizado e Turbo habilitado |
| `GIF_PROFILE_BALANCED` | Prefixo 16-bit + sufixo 8-bit, `FILE_BUF=768`, `LZW_BUF=768`, sem Turbo |
| `GIF_PROFILE_MINIMAL` | Prefixo 12-bit + sufixo 8-bit, `FILE_BUF=768`, `LZW_BUF=512`, `RGB565_ONLY`, sem Turbo |
| `GIF_PROFILE_COOPERATIVE` | Base Balanced + decodificação incremental cooperativa |
| `GIF_PROFILE_TRANSIENT` | Struct residente de ~`664 B`; aloca tabelas de 16-bit + `LZW_BUF=1530` por quadro no heap |

Permitir opções ortogonais (`GIF_DICT_MODE`, `GIF_FILE_BUF_SIZE`, `GIF_LZW_BUF_SIZE`, `GIF_RGB565_ONLY`, `GIF_ENABLE_COOPERATIVE`, `GIF_LAZY_RESET`, `GIF_TRANSIENT_WORK`, `GIF_MALLOC`/`GIF_CALLOC`/`GIF_FREE`, `MAX_WIDTH`) protegendo combinações incompatíveis com `#error` (ex.: `GIF_DISABLE_TURBO == 0` com dicionário compacto ou modo transiente). No wrapper C++, construtor padrão zera `_gif`, destrutor RAII chama `close()`, cópia/atribuição são desabilitadas (`= delete`), `close()`/`GIF_close()` são idempotentes e `pfnClose` só permanece ativo se `pfnOpen`/`fopen` tiver sucesso. As macros leitoras little-endian usam o prefixo `GIF_INTELSHORT` / `GIF_INTELLONG` para evitar colisão com headers irmãos (como `JPEGDEC.h`). O perfil padrão (`GIF_PROFILE_FAST`) conserva o comportamento compatível do upstream com as correções de robustez e lazy reset.

## 9. Benchmark e critérios de aceitação

Plataformas validadas nesta etapa: ESP8266 (`esp8266:esp8266 3.1.2` via `arduino-cli`) e host desktop (WSL x86_64 e x86 `-m32` com GCC 15 + ASan/UBSan).

| Métrica | Método |
|---|---|
| RAM estática | `sizeof(GIFIMAGE)`, `sizeof(AnimatedGIF)` e relatório de linker (`DATA+RODATA+BSS`) |
| Flash | Tamanho binário compilado com mesma configuração |
| Desempenho | Tempo de decodificação pura por frame (callback nulo), mediana e p95 |
| Responsividade | Maior intervalo único em decodificação cooperativa (`decodeStep` / `decodeLines`) |
| Correção | Hash FNV-1a pixel-a-pixel contra o upstream `17a6a2ed` + fuzzing ASan/UBSan |

### Resultados empíricos (v1 experimental)

#### 1. Memória e Flash no ESP8266 (`arduino-cli`)

Compilado para `esp8266:esp8266:generic:eesz=4M2M,xtal=160,ip=lm2n` via [`tests/esp8266_measure.ps1`](../tests/esp8266_measure.ps1) (no ESP8266, `MAX_WIDTH` padrão é `480` em `src/AnimatedGIF.h` quando não sobrescrito):

| Perfil / Flags | `sizeof(AnimatedGIF)` | Delta vs Upstream | `DRAM` (`DATA+RODATA+BSS`) | `IRAM` | `IROM` (Flash) | `.bin` |
|---|---:|---:|---:|---:|---:|---:|
| Upstream (`17a6a2ed`) | 24172 B | — | 52228 B (`49792` BSS) | 26979 B | 232292 B | 265856 B |
| `GIF_PROFILE_FAST` (padrão) | 24172 B | 0 B | 52232 B (`49792` BSS) | 26979 B | 232676 B | 266240 B |
| `GIF_PROFILE_BALANCED` | 15980 B | -8192 B (-33,9%) | 44040 B (`41600` BSS) | 26979 B | 231236 B | 264800 B |
| `GIF_PROFILE_MINIMAL` | 13164 B | -11008 B (-45,5%) | 41224 B (`38784` BSS) | 26979 B | 230372 B | 263936 B |
| `GIF_PROFILE_MINIMAL -DMAX_WIDTH=240` | 12924 B | -11248 B (-46,5%) | 40984 B (`38544` BSS) | 26979 B | 230372 B | 263936 B |
| `GIF_PROFILE_COOPERATIVE` | 16000 B | -8172 B (-33,8%) | 44064 B (`41624` BSS) | 26991 B | 231556 B | 265136 B |
| `GIF_PROFILE_MINIMAL -DGIF_ENABLE_COOPERATIVE=1` | 13184 B | -10988 B (-45,5%) | 41248 B (`38808` BSS) | 26991 B | 230676 B | 264256 B |
| `GIF_PROFILE_TRANSIENT` | 664 B | -23508 B (-97,3%) | 28728 B (`26288` BSS) | 26979 B | 230436 B | 264000 B |
| `GIF_PROFILE_TRANSIENT -DGIF_FILE_BUF_SIZE=4096` | 664 B | -23508 B (-97,3%) | 28728 B (`26288` BSS) | 26979 B | 230452 B | 264016 B |
| `GIF_PROFILE_TRANSIENT -DGIF_ENABLE_COOPERATIVE=1` | 684 B | -23488 B (-97,2%) | 28744 B (`26304` BSS) | 26991 B | 230756 B | 264336 B |

> No `GIF_PROFILE_TRANSIENT`, apenas `664 B` (`684 B` com cooperativo) ficam residentes entre quadros; os buffers de decodificação somam pico transiente de `15593 B` no padrão (`18921 B` com `GIF_FILE_BUF_SIZE=4096`, maior bloco contíguo de `8192 B`) apenas enquanto o quadro é decodificado, com `0 B` no heap durante `open()` / `getInfo()` (lote de paleta de 48 B na pilha). Todos os demais perfis fazem `0` alocações dinâmicas.

#### 2. Desempenho e responsividade no host (`-O2`, x86_64 WSL2, `MAX_WIDTH=2048`)

Comparação aquecida (`tests/bench.cpp`, callback nulo) entre perfis principais:

| Alvo (frames) | `FAST` (25800 B) | `FAST_LAZY0` (25800 B) | `BALANCED` (17600 B) | `MINIMAL` (14784 B) |
|---|---:|---:|---:|---:|
| `emb_badgers` (9f) | 2,15 ms (med 216,50 us) | 2,34 ms (med 226,00 us) | 3,11 ms (med 339,40 us) | 4,85 ms (med 515,90 us) |
| `emb_bw_wiggler` (200f) | 12,91 ms (med 56,50 us) | 13,46 ms (med 52,40 us) | 16,40 ms (med 69,30 us) | 32,32 ms (med 143,90 us) |
| `emb_earth` (103f) | 38,19 ms (med 353,00 us) | 40,66 ms (med 374,40 us) | 43,14 ms (med 367,90 us) | 49,96 ms (med 472,70 us) |
| `emb_nostromo` (202f) | 25,46 ms (med 107,10 us) | 40,54 ms (med 148,80 us) | 33,92 ms (med 143,00 us) | 49,15 ms (med 215,00 us) |
| `emb_thisisfine` (33f) | 22,06 ms (med 609,30 us) | 21,45 ms (med 564,70 us) | 24,13 ms (med 669,50 us) | 39,53 ms (med 1194,80 us) |
| `valid_deferred_cc` (1f) | 0,82 ms | 1,33 ms | 1,49 ms | 1,46 ms |
| `valid_multi_cc` (1f) | 0,05 ms | 0,05 ms | 0,05 ms | 0,13 ms |
| `valid_long_chain` (1f, cadeia 1238 px) | 2,19 ms | 2,30 ms | 4,38 ms | 9,11 ms |

- **Lazy reset isolado (A/B sem alteração estrutural no commit `a077867`):** eliminar o `memset` no Clear Code reduziu o tempo total em `-13,5%` (`emb_bw_wiggler` `6,52 -> 5,64 ms`), `-10,5%` (`emb_badgers` `1,24 -> 1,11 ms`), `-7,9%` (`emb_nostromo` `11,20 -> 10,31 ms`) e `-6,1%` (`emb_earth` `13,83 -> 12,99 ms`).
- **Varredura de `GIF_LZW_BUF_SIZE` sobre `MINIMAL` (host ocioso, entrada em RAM):** `264 B` (`sizeof=14536`, `emb_earth=44,62 ms`, `emb_bw_wiggler=27,44 ms`, `valid_long_chain=8,74 ms`), `512 B` (`sizeof=14784`, `38,96 ms` / `26,08 ms` / `9,19 ms`) e `1530 B` (`sizeof=15808`, `38,03 ms` / `28,07 ms` / `9,76 ms`). No host em RAM a variação de ~15–30% por agendamento/turbo do WSL2 domina porque callbacks em memória têm custo quase nulo; em flash/LittleFS/SD, buffers maiores reduzem trocas de contexto de I/O.
- **Limitação de largura (`MAX_WIDTH=240`):** no perfil `MINIMAL_240` (`sizeof=12976` no host), `valid_long_chain.gif` é rejeitado na abertura (`frames=0`) porque sua largura excede `MAX_WIDTH=240`.
- **Latência cooperativa (`decodeStep(500)`, orçamento de 500 us):**

| Perfil cooperativo | `sizeof` (host 64-bit) | Faixa de `max_step` | `valid_long_chain` `max_step` vs frame completo | `emb_earth` total / `max_step` |
|---|---:|---:|---:|---:|
| `GIF_PROFILE_COOPERATIVE` | 17624 B | 47,80–593,40 us | 593,40 us (vs 3605,61 us med) | 30,04 ms / 523,40 us |
| `GIF_PROFILE_MINIMAL + COOP` | 14808 B | 85,90–621,70 us | 515,90 us (vs 8317,53 us med) | 37,83 ms / 599,80 us |

- **Comparação direta no host ocioso (`-O2`, x86_64 WSL2) com `TRANSIENT`:**

| Alvo (frames) | `FAST` (25800 B) | `MINIMAL` (14784 B) | `TRANSIENT` (744 B res.) | `TRANSIENT_4K` (744 B res.) | `TRANSIENT_COOP` (768 B, `max_step`) |
|---|---:|---:|---:|---:|---:|
| `emb_badgers` (9f) | 2,06 ms | 1,82 ms | 1,19 ms | 1,24 ms | 1,35 ms (`171,69 us`) |
| `emb_bw_wiggler` (200f) | 10,92 ms | 14,54 ms | 6,36 ms | 7,12 ms | 7,64 ms (`94,00 us`) |
| `emb_earth` (103f) | 22,30 ms | 19,36 ms | 14,66 ms | 17,82 ms | 14,51 ms (`317,49 us`) |
| `emb_nostromo` (202f) | 12,82 ms | 23,35 ms | 12,79 ms | 11,99 ms | 12,05 ms (`178,89 us`) |
| `emb_thisisfine` (33f) | 9,46 ms | 16,15 ms | 9,20 ms | 9,45 ms | 11,79 ms (`342,68 us`) |
| `valid_deferred_cc` (1f) | 0,43 ms | 0,49 ms | 0,36 ms | 0,35 ms | 0,60 ms (`463,08 us`) |
| `valid_multi_cc` (1f) | 0,02 ms | 0,06 ms | 0,02 ms | 0,02 ms | 0,02 ms (`29,00 us`) |
| `valid_long_chain` (1f) | 1,09 ms | 4,75 ms | 2,16 ms | 1,42 ms | 2,19 ms (`503,48 us`) |

#### 3. Correção, fuzzing e integração drop-in no `smalltv-pomodoro`

- **Suíte de regressão e fuzzing (`tests/run_tests.sh`):**
  - `run_tests.sh 25000`: todas as 17 configurações (`fast`, `fast32`, `fast_no_lazy`, `balanced`, `minimal`, `minimal240`, `coop`, `minimal_coop`, `dict16_full`, `dict12_full`, `dict12_lzw264`, `transient`, `transient32`, `transient_coop`, `transient_dict0`, `transient_dict12`, `transient_4k`) passaram com `0` falhas (44 checks nos perfis estáticos; 48 checks nos perfis transientes incluindo contadores exatos de heap com preenchimento `0xA5`, injeção determinística de OOM, recuperação pós-OOM, aborto cooperativo por `reset()`/`~AnimatedGIF()`, `close()` idempotente, `open()` com falha e LeakSanitizer ativo), duas checagens em C99 puro com `-Wall -Wextra`, e fuzzing ASan+UBSan (`25000` casos em `minimal+coop` 32 bits + `25000` casos em `fast` + `25000` casos em `transient+coop` 32 bits, além de `5000` casos 64 bits com LeakSanitizer `detect_leaks=1`) com `0` vazamentos e `0` falhas.
- **Integração drop-in no [`smalltv-pomodoro`](https://github.com/davicorrea0/smalltv-pomodoro)** (`GIF_PROFILE_MINIMAL` + `MAX_WIDTH 480` via `AnimatedGIF_config.h`, `gif_inl.h` no `src/` do sketch, `album.cpp` inalterado):

| Métrica no `smalltv-pomodoro` | Baseline | Com `AnimatedGIF-LowMem` | Diferença |
|---|---:|---:|---:|
| `sizeof(AnimatedGIF)` (32 bits) | 24172 B | 13164 B | **-11008 B (-45,5% por alocação no heap)** |
| RAM estática (`DATA+RODATA+BSS`) | 38420 B | 38420 B | 0 B (`AnimatedGIF` é alocado sob demanda no heap) |
| Flash `IROM` | 544340 B | 542692 B | -1648 B |
| Binário `.bin` / `.bin.gz` | 585248 B / 431538 B | 583600 B / 430287 B | -1648 B / -1251 B |
| `sim/test-gif.sh` (`ff_240`, `ff_small`, 5 maliciosos) | `81996386`/`53363a1a`, `b2d1cdd0`/`6ac915c3` | Idêntico (bit a bit) | 0 regressões + 200k fuzz ASan OK |
| `sim/testes.sh` (handlers, telemetria, 5k GIF/JPEG fuzz) | OK | OK | 0 falhas |

## 10. Roadmap de implementação

- [x] Criar fork separado (`davicorrea0/AnimatedGIF-LowMem`) e fixar commit de referência (`upstream-baseline`).
- [x] Criar corpus sintético + regressão golden + harness de fuzzing e benchmark reproduzível.
- [x] Validar limites, leituras parciais, `GIF_getInfo()`, `getComment()` e propagação de erros.
- [x] Implementar lazy reset e medir isoladamente.
- [x] Implementar dicionário prefixo/sufixo de 16 bits e fallback de pilha em blocos.
- [x] Implementar prefixos compactados de 12 bits e redução de buffers/paleta.
- [x] Implementar decodificação cooperativa opcional.
- [x] Comparar estratégias de buffer de entrada e perfis no host e no ESP8266.
- [x] Validar integração drop-in no `smalltv-pomodoro`.
- [x] Documentar perfis, limitações e benchmarks reais em `README.md` e nesta proposta.

## 11. Referências

- [AnimatedGIF — repositório upstream](https://github.com/bitbank2/AnimatedGIF)
- [AnimatedGIF-LowMem — fork experimental](https://github.com/davicorrea0/AnimatedGIF-LowMem)
- [SmallTV Pomodoro — ambiente de validação em ESP8266](https://github.com/davicorrea0/smalltv-pomodoro)

## 12. Status e limitações

- **Commit de referência:** `17a6a2eda4385c14f39ee51adfc89ab721150527` (tag `upstream-baseline`).
- **Status v1 experimental:** implementação concluída e verificada no host (GCC ASan/UBSan x86_64/i386 + `-O2`), no ESP8266 (`arduino-cli` `3.1.2`) e na integração drop-in com o `smalltv-pomodoro`.
- **Limitações conhecidas e compromissos:**
  - **Turbo vs dicionário compacto:** `GIF_DICT_MODE != 0`, `GIF_FILE_BUF_SIZE < 4096`, `GIF_LZW_BUF_SIZE < 1530` e `GIF_RGB565_ONLY=1` são incompatíveis com o layout contíguo do `DecodeLZWTurbo()` e exigem `GIF_DISABLE_TURBO=1` (protegido por `#error`).
  - **`GIF_RGB565_ONLY=1`:** economiza `512 B` nas paletas mas recusa `GIF_PALETTE_RGB888`.
  - **Cadeias longas com `GIF_FILE_BUF_SIZE` reduzido:** cadeias maiores que `768 B` acionam `GIFEmitChainChunked` em múltiplos passos; combinado com prefixos compactados de 12 bits (`MINIMAL`), eleva o tempo em casos sintéticos extremos como `valid_long_chain` (`~2,19 ms -> ~9,11 ms` no host), embora imagens reais típicas mantenham latência moderada e o modo cooperativo limite cada fatia a ~0,5–0,6 ms.
  - **Orçamento de `decodeStep(budgetUs)`:** é um orçamento cooperativo verificável apenas em fronteiras seguras (entre códigos/linhas), não uma garantia de tempo real em torno de callbacks lentos de SPI/LittleFS/display.
  - **Vendoring dentro de `sketch/src/` no `arduino-cli`:** a etapa de preparação de sketch do `arduino-cli` pode não copiar arquivos `.inl`; ao copiar para `sketch/src/AnimatedGIF/` (em vez de usar `--library`), renomear `gif.inl` para `gif_inl.h` (o fallback `__has_include` em `src/AnimatedGIF.cpp` resolve automaticamente).
  - **Compatibilidade com `JPEGDEC.h`:** resolvida com a renomeação das macros leitoras para `GIF_INTELSHORT` / `GIF_INTELLONG` em `src/AnimatedGIF.h` (validado sem avisos sob `-Wall -Wextra -Werror`).
