#ifndef GUARD_BATTLE_MAIN_H
#define GUARD_BATTLE_MAIN_H

// Minimal mock for native unit tests.
// Replaces include/battle_main.h to avoid pulling in pokemon.h.

struct MultiPartnerMenuPokemon {
    /*0x00*/ u16 species;
    /*0x02*/ u16 heldItem;
    /*0x04*/ u8  nickname[POKEMON_NAME_LENGTH + 1];
    /*0x0F*/ u8  level;
    /*0x10*/ u16 hp;
    /*0x12*/ u16 maxhp;
    /*0x14*/ u32 status;
    /*0x18*/ u32 personality;
    /*0x1C*/ u8  gender;
    /*0x1D*/ u8  language;
};

extern struct MultiPartnerMenuPokemon gMultiPartnerParty[MULTI_PARTY_SIZE];

#endif // GUARD_BATTLE_MAIN_H
