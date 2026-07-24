/*
 * override_stubs.c — Sistema de overrides por endereço PS2
 * =========================================================
 * Permite interceptar funções específicas do jogo por PC sem rebuildar output.c.
 *
 * Analogia ao game_overrides.cpp do projeto godofwar:
 *   - Deles: registry por endereço com R5900Context* ctx
 *   - Nosso: tabela estática de overrides que o dispatcher consulta
 *
 * Como usar:
 *   1. Adicionar uma entrada em ps2_override_table[] abaixo
 *   2. Implementar a função override_ADDR() neste arquivo
 *   3. Rebuildar só o runtime (não o output.c) via build_relink.sh
 *
 * O dispatcher em output_runtime.c consulta ps2_find_override(pc) antes
 * de chamar a função recompilada. Se retornar não-NULL, chama o override.
 *
 * REGRA ANTI-FALSO-POSITIVO:
 *   Um override que force-retorna um valor sem executar o código PS2 nativo
 *   NÃO é progresso. Só é válido se:
 *     a) A função real não existe no range recompilado (gap), OU
 *     b) O override implementa o comportamento correto baseado em análise
 *        do binário PS2 (não apenas força v0=0 para passar um check).
 *
 *   Critério de "progresso real":
 *     triage_headless.py: frames_completados > 0 por código PS2 nativo
 */

#include "../include/ps2_runtime.h"
#include <stdio.h>
#include <stdint.h>

/* -----------------------------------------------------------------------
 * Tipo de função override
 * ----------------------------------------------------------------------- */
typedef void (*OverrideFn)(PS2Regs* regs);

typedef struct {
    uint32_t   pc;       /* Endereço PS2 da função a interceptar */
    OverrideFn fn;       /* Handler a chamar em vez da função recompilada */
    const char* name;    /* Nome legível para diagnóstico */
} OverrideEntry;

/* -----------------------------------------------------------------------
 * Forward declarations das funções override
 * Adicione aqui ao implementar novas
 * ----------------------------------------------------------------------- */
/* (nenhum ativo — ver __wrap_func_21ff00 abaixo para o fix VBlank) */

/* -----------------------------------------------------------------------
 * Tabela de overrides
 * Ordenada por pc para busca binária — manter ordenada!
 * ----------------------------------------------------------------------- */
static const OverrideEntry ps2_override_table[] = {
    /* Exemplos comentados — descomente e implemente conforme necessário:
    *
    * { 0x00100000u, override_entry_point,    "entry_point"     },
    * { 0x001234ABu, override_some_func,      "some_func"       },
    */
};

/* -----------------------------------------------------------------------
 * __wrap_func_21ff00 — fix para loop infinito de VBlank (via --wrap do linker)
 *
 * Diagnóstico:
 *   func_21ff00 [0x21ff00-0x21ff58] é um wait-for-VBlank que lê
 *   mem[0x29C7D4] (contador de VBlanks), gira ~20000 ciclos, e relê.
 *   Se o contador não mudou → loop externo (L_21ff20).
 *   BUG do recompilador: o delay slot (addiu v0, 20000) sobrescreve o
 *   resultado do slt ANTES do check do bne, tornando a branch sempre
 *   tomada. O loop nunca termina mesmo com VBlank simulado.
 *
 * Fix: substituir a função inteira. Incrementamos mem[0x29C7D4] (simula
 * um VBlank firing) e retornamos imediatamente. Isso satisfaz qualquer
 * outro código que aguarde esse contador e mantém a lógica correta.
 *
 * REGRA ANTI-FALSO-POSITIVO: esta função é um delay/busy-wait de HW.
 * Substituí-la não avança state machine artificialmente — apenas remove
 * um spin que depende de interrupção que nosso runtime nunca dispara.
 * ----------------------------------------------------------------------- */
void __wrap_func_21ff00(PS2Regs* regs) {
    /* Incrementa o contador de VBlank para que quem aguardar via slt veja
     * o valor atualizado e saia do loop normalmente. */
    uint32_t vblank_addr = 0x29C7D4u;
    uint32_t cnt = mem_read32(vblank_addr);
    mem_write32(vblank_addr, cnt + 1);
    /* Sem spin — retorna imediatamente. */
    (void)regs;
}

static const int PS2_OVERRIDE_COUNT =
    (int)(sizeof(ps2_override_table) / sizeof(ps2_override_table[0]));

/* -----------------------------------------------------------------------
 * Lookup — busca binária por PC
 * Retorna NULL se não houver override para este PC
 * ----------------------------------------------------------------------- */
OverrideFn ps2_find_override(uint32_t pc) {
    int lo = 0, hi = PS2_OVERRIDE_COUNT - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (ps2_override_table[mid].pc == pc)
            return ps2_override_table[mid].fn;
        if (ps2_override_table[mid].pc < pc)
            lo = mid + 1;
        else
            hi = mid - 1;
    }
    return NULL;
}

/* -----------------------------------------------------------------------
 * Init — imprime tabela de overrides ativos no boot
 * Chamar de host_main.cpp antes de ps2_game_start()
 * ----------------------------------------------------------------------- */
void ps2_overrides_init(void) {
    if (PS2_OVERRIDE_COUNT == 0) {
        fprintf(stderr, "[OVERRIDE] Nenhum override ativo.\n");
        return;
    }
    fprintf(stderr, "[OVERRIDE] %d override(s) ativo(s):\n", PS2_OVERRIDE_COUNT);
    for (int i = 0; i < PS2_OVERRIDE_COUNT; i++) {
        fprintf(stderr, "  [%d] 0x%08x → %s\n",
            i, ps2_override_table[i].pc, ps2_override_table[i].name);
    }
}

/* -----------------------------------------------------------------------
 * Implementações de override
 *
 * Padrão:
 *   static void override_ADDR(PS2Regs* regs) {
 *       // Implementação baseada em análise do binário PS2
 *       // Escrever em regs->r[2] (v0) = valor de retorno
 *       // NUNCA force-return sem análise — ver REGRA ANTI-FALSO-POSITIVO acima
 *       fprintf(stderr, "[OVERRIDE 0x%08x] chamado pc=0x%08x\n",
 *               0xADDRu, regs->pc);
 *   }
 * ----------------------------------------------------------------------- */

/* (implementações aqui conforme necessário) */
