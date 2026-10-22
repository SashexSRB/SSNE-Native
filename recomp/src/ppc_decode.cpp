#include "ppc_decode.h"

namespace ppc {

std::string mnemonic(uint32_t i) {
    switch (OPCD(i)) {
    case 3: return "twi"; case 4: return "ps"; case 7: return "mulli"; case 8: return "subfic";
    case 10: return "cmpli"; case 11: return "cmpi"; case 12: return "addic"; case 13: return "addic.";
    case 14: return "addi"; case 15: return "addis"; case 16: return "bc"; case 17: return "sc";
    case 18: return "b"; case 19: return "op19"; case 20: return "rlwimi"; case 21: return "rlwinm";
    case 23: return "rlwnm"; case 24: return "ori"; case 25: return "oris"; case 26: return "xori";
    case 27: return "xoris"; case 28: return "andi."; case 29: return "andis."; case 31: return "op31";
    case 32: return "lwz"; case 33: return "lwzu"; case 34: return "lbz"; case 35: return "lbzu";
    case 36: return "stw"; case 37: return "stwu"; case 38: return "stb"; case 39: return "stbu";
    case 40: return "lhz"; case 41: return "lhzu"; case 42: return "lha"; case 43: return "lhau";
    case 44: return "sth"; case 45: return "sthu"; case 46: return "lmw"; case 47: return "stmw";
    case 48: return "lfs"; case 49: return "lfsu"; case 50: return "lfd"; case 51: return "lfdu";
    case 52: return "stfs"; case 53: return "stfsu"; case 54: return "stfd"; case 55: return "stfdu";
    case 56: return "psq_l"; case 57: return "psq_lu"; case 59: return "op59"; case 60: return "psq_st";
    case 61: return "psq_stu"; case 63: return "op63";
    }
    return "op" + std::to_string(OPCD(i));
}

} // namespace ppc
