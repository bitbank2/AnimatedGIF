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

- `GIFMakePels()`: o limite `LINK_UNUSED` (5911) é superior à capacidade física do dicionário padrão (4096). Verificar cadeias inválidas e validar limites físicos e lógicos antes de acessar tabelas. (Confirmado no upstream `17a6a2ed`: `code` entre 4096 e 5910 causa leitura fora dos limites em `giftabs`/`gifpels`, reproduzível com ASan).
- `GIFGetMoreData()`: verificar os retornos de `pfnRead`, inclusive leituras parciais (fazendo loop enquanto retornar `> 0`), EOF (`0`) e erros (`<= 0`); nunca contabilizar bytes não lidos em `iLZWSize`.
- Leitor de bits (`GET_CODE` / `GET_CODE_TURBO`): garantir que exista informação suficiente para cada código e para cada leitura de palavra, inclusive nas bordas dos buffers, eliminando leituras de 4/8 bytes além do fim de `ucLZW`.
- Propagar falhas de expansão em vez de retornar silenciosamente de `GIFMakePels()`.
- Validar dimensões, offsets e operações de tamanho de alocação (`allocFrameBuf`, `allocTurboBuf`) com aritmética verificada.
- Fazer `nextcode` saturar quando o dicionário estiver cheio (`1 << MAX_CODE_SIZE`); testar fluxos com Clear Code diferido.
- **Achados adicionais na validação estática:** `GIFParseInfo()`, `GIF_getInfo()` e `getComment()` no upstream também executam leituras e indexações sem verificação de limites (especialmente com arquivos truncados, sub-blocos malformados e `iSize` inconsistente); no upstream, `DecodeLZW()` também não drena sub-blocos restantes ao encerrar por `iYCount == 0` antes do `EOI`, dessincronizando quadros seguintes.

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

Permitir opções ortogonais (`GIF_DICT_MODE`, `GIF_FILE_BUF_SIZE`, `GIF_LZW_BUF_SIZE`, `GIF_RGB565_ONLY`, `GIF_ENABLE_COOPERATIVE`, `GIF_LAZY_RESET`, `MAX_WIDTH`) protegendo combinações incompatíveis com `#error` (ex.: `GIF_DISABLE_TURBO == 0` com dicionário compacto). O perfil padrão (`GIF_PROFILE_FAST`) conserva o comportamento compatível do upstream com as correções de robustez e lazy reset.

## 9. Benchmark e critérios de aceitação

Plataformas validadas nesta etapa: ESP8266 (`esp8266:esp8266 3.1.2` via `arduino-cli`) e host desktop (WSL x86_64 e x86 `-m32` com GCC 15 + ASan/UBSan).

| Métrica | Método |
|---|---|
| RAM estática | `sizeof(GIFIMAGE)`, `sizeof(AnimatedGIF)` e relatório de linker (`DATA+RODATA+BSS`) |
| Flash | Tamanho binário compilado com mesma configuração |
| Desempenho | Tempo de decodificação pura por frame (callback nulo), mediana e p95 |
| Responsividade | Maior intervalo único em decodificação cooperativa (`decodeStep` / `decodeLines`) |
| Correção | Hash FNV-1a pixel-a-pixel contra o upstream `17a6a2ed` + fuzzing ASan/UBSan |

### Metas experimentais

- Reduzir a memória do dicionário em 4–6 KiB.
- Investigar consumo total de buffers de 12–16 KiB com otimizações adicionais (mantendo 12 bits completos).
- Preservar suporte a códigos de 12 bits e comportamento da API existente.
- Não introduzir regressões de pixels nos recursos suportados.
- Quantificar qualquer penalidade de velocidade e ganho de responsividade.

## 10. Roadmap de implementação

- [x] Criar fork separado (`davicorrea0/AnimatedGIF-LowMem`) e fixar commit de referência (`upstream-baseline`).
- [ ] Criar corpus sintético + regressão golden + harness de fuzzing e benchmark reproduzível.
- [ ] Validar limites, leituras parciais, `GIF_getInfo()`, `getComment()` e propagação de erros.
- [ ] Implementar lazy reset e medir isoladamente.
- [ ] Implementar dicionário prefixo/sufixo de 16 bits e fallback de pilha em blocos.
- [ ] Implementar prefixos compactados de 12 bits e redução de buffers/paleta.
- [ ] Implementar decodificação cooperativa opcional.
- [ ] Comparar estratégias de buffer de entrada e perfis no host e no ESP8266.
- [ ] Validar integração drop-in no `smalltv-pomodoro`.
- [ ] Documentar perfis, limitações e benchmarks reais em `README.md` e nesta proposta.

## 11. Referências

- [AnimatedGIF — repositório upstream](https://github.com/bitbank2/AnimatedGIF)
- [AnimatedGIF-LowMem — fork experimental](https://github.com/davicorrea0/AnimatedGIF-LowMem)
- [SmallTV Pomodoro — ambiente de validação em ESP8266](https://github.com/davicorrea0/smalltv-pomodoro)

## 12. Status e limitações

- **Commit de referência:** `17a6a2eda4385c14f39ee51adfc89ab721150527`.
- Esta versão do documento registra a validação estática inicial; os resultados empíricos de compilação, RAM, Flash, fuzzing e benchmark serão adicionados após a execução das etapas experimentais.
