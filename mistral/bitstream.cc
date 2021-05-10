/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2021  gatecat <gatecat@ds0.me>
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include "log.h"
#include "nextpnr.h"
#include "util.h"

NEXTPNR_NAMESPACE_BEGIN
namespace {
struct MistralBitgen
{
    MistralBitgen(Context *ctx) : ctx(ctx), cv(ctx->cyclonev){};
    Context *ctx;
    CycloneV *cv;

    void init()
    {
        ctx->init_base_bitstream();
        // Default options
        cv->opt_b_set(CycloneV::ALLOW_DEVICE_WIDE_OUTPUT_ENABLE_DIS, true);
        cv->opt_n_set(CycloneV::CRC_DIVIDE_ORDER, 8);
        cv->opt_b_set(CycloneV::CVP_CONF_DONE_EN, true);
        cv->opt_b_set(CycloneV::DEVICE_WIDE_RESET_EN, true);
        cv->opt_n_set(CycloneV::DRIVE_STRENGTH, 8);
        cv->opt_b_set(CycloneV::IOCSR_READY_FROM_CSR_DONE_EN, true);
        cv->opt_b_set(CycloneV::NCEO_DIS, true);
        cv->opt_b_set(CycloneV::OCT_DONE_DIS, true);
        cv->opt_r_set(CycloneV::OPT_A, 0x1dff);
        cv->opt_r_set(CycloneV::OPT_B, 0xffffff402dffffffULL);
        cv->opt_b_set(CycloneV::RELEASE_CLEARS_BEFORE_TRISTATES_DIS, true);
        cv->opt_b_set(CycloneV::RETRY_CONFIG_ON_ERROR_EN, true);
        cv->opt_r_set(CycloneV::START_UP_CLOCK, 0x3F);
        // Default inversion
        write_default_inv();
    }

    void write_default_inv()
    {
        // Some PNODEs are inverted by default. Set them up here.
        for (const auto &pn2r : cv->get_all_p2r()) {
            const auto &pn = pn2r.first;
            auto pt = CycloneV::pn2pt(pn);
            auto pi = CycloneV::pn2pi(pn);

            switch (CycloneV::pn2bt(pn)) {
            case CycloneV::HMC: {
                // HMC OE are inverted to set OE=0, i.e. unused pins floating
                // TODO: handle the case when we are using the HMC or HMC bypass
                std::string name(CycloneV::port_type_names[pt]);
                if (name.compare(0, 5, "IOINT") != 0 || name.compare(name.size() - 2, 2, "OE") != 0)
                    continue;
                cv->inv_set(pn2r.second, true);
                break;
            };
            // HPS IO - TODO: what about when we actually support the HPS primitives?
            case CycloneV::HPS_BOOT: {
                switch (pt) {
                case CycloneV::CSEL_EN:
                case CycloneV::BSEL_EN:
                case CycloneV::BOOT_FROM_FPGA_READY:
                case CycloneV::BOOT_FROM_FPGA_ON_FAILURE:
                    cv->inv_set(pn2r.second, true);
                    break;
                case CycloneV::CSEL:
                    if (pi < 2)
                        cv->inv_set(pn2r.second, true);
                    break;
                case CycloneV::BSEL:
                    if (pi < 3)
                        cv->inv_set(pn2r.second, true);
                    break;
                default:
                    break;
                };
                break;
            };
            case CycloneV::HPS_CROSS_TRIGGER: {
                if (pt == CycloneV::CLK_EN)
                    cv->inv_set(pn2r.second, true);
                break;
            };
            case CycloneV::HPS_TEST: {
                if (pt == CycloneV::CFG_DFX_BYPASS_ENABLE)
                    cv->inv_set(pn2r.second, true);
                break;
            };
            case CycloneV::GPIO: {
                // Ignore GPIO used by the design
                BelId bel = ctx->bel_by_block_idx(CycloneV::pn2x(pn), CycloneV::pn2y(pn), id_MISTRAL_IO,
                                                  CycloneV::pn2bi(pn));
                if (bel != BelId() && ctx->getBoundBelCell(bel) != nullptr)
                    continue;
                // Bonded IO invert OEIN.1 which disables the output buffer and floats the IO
                // Unbonded IO invert OEIN.0 which enables the output buffer, and {DATAIN.[0123]} to drive a constant
                // GND, presumably for power/EMI reasons
                bool is_bonded = cv->pin_find_pnode(pn) != nullptr;
                if (is_bonded && (pt != CycloneV::OEIN || pi != 1))
                    continue;
                if (!is_bonded && (pt != CycloneV::DATAIN) && (pt != CycloneV::OEIN || pi != 0))
                    continue;
                cv->inv_set(pn2r.second, true);
                break;
            };
            case CycloneV::FPLL: {
                if (pt == CycloneV::EXTSWITCH || (pt == CycloneV::CLKEN && pi < 2))
                    cv->inv_set(pn2r.second, true);
                break;
            };
            default:
                break;
            }
        }
    }

    void write_dqs()
    {
        for (auto pos : cv->dqs16_get_pos()) {
            int x = CycloneV::pos2x(pos), y = CycloneV::pos2y(pos);
            // DQS bypass for used output pins
            for (int z = 0; z < 16; z++) {
                int ioy = y + (z / 4) - 2;
                if (ioy < 0 || ioy >= int(cv->get_tile_sy()))
                    continue;
                BelId bel = ctx->bel_by_block_idx(x, ioy, id_MISTRAL_IO, z % 4);
                if (bel == BelId())
                    continue;
                CellInfo *ci = ctx->getBoundBelCell(bel);
                if (ci == nullptr || (ci->type != id_MISTRAL_IO && ci->type != id_MISTRAL_OB))
                    continue; // not an output
                cv->bmux_m_set(CycloneV::DQS16, pos, CycloneV::INPUT_REG4_SEL, z, CycloneV::SEL_LOCKED_DPA);
                cv->bmux_r_set(CycloneV::DQS16, pos, CycloneV::RB_T9_SEL_EREG_CFF_DELAY, z, 0x1f);
            }
        }
    }

    void write_routing()
    {
        for (auto net : sorted(ctx->nets)) {
            NetInfo *ni = net.second;
            for (auto wire : sorted_ref(ni->wires)) {
                PipId pip = wire.second.pip;
                if (pip == PipId())
                    continue;
                WireId src = ctx->getPipSrcWire(pip), dst = ctx->getPipDstWire(pip);
                // Only write out routes that are entirely in the Mistral domain. Everything else is dealt with
                // specially
                if (src.is_nextpnr_created() || dst.is_nextpnr_created())
                    continue;
                cv->rnode_link(src.node, dst.node);
            }
        }
    }

    void run()
    {
        cv->clear();
        init();
        write_routing();
        write_dqs();
    }
};
} // namespace

void Arch::build_bitstream()
{
    MistralBitgen gen(getCtx());
    gen.run();
}

NEXTPNR_NAMESPACE_END
