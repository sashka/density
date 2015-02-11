/*
 * Centaurean Density
 *
 * Copyright (c) 2015, Guillaume Voirin
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in the
 *       documentation and/or other materials provided with the distribution.
 *     * Neither the name of Centaurean nor the
 *       names of its contributors may be used to endorse or promote products
 *       derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 * 9/02/15 19:22
 *
 * ------------------
 * Argonaut algorithm
 * ------------------
 *
 * Author(s)
 * Guillaume Voirin (https://github.com/gpnuma)
 *
 * Description
 * Multiform compression algorithm
 */

#include "kernel_argonaut_encode.h"
#include "memory_location.h"
#include "kernel_argonaut_dictionary.h"

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE exitProcess(density_argonaut_encode_state *state, DENSITY_ARGONAUT_ENCODE_PROCESS process, DENSITY_KERNEL_ENCODE_STATE kernelEncodeState) {
    state->process = process;
    return kernelEncodeState;
}

DENSITY_FORCE_INLINE void density_argonaut_encode_push_to_output(density_memory_location *restrict out, uint16_t content) {
    *(uint16_t *) out->pointer = content;
    out->pointer += sizeof(uint16_t);
    out->available_bytes -= sizeof(uint16_t);
}

DENSITY_FORCE_INLINE void density_argonaut_encode_prepare_new_signature(density_memory_location *restrict out, density_argonaut_encode_state *restrict state) {
    state->shift = 0;
    state->signature = (density_argonaut_signature *) (out->pointer);
    *state->signature = 0;

    out->pointer += sizeof(density_argonaut_signature);
    out->available_bytes -= sizeof(density_argonaut_signature);
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_argonaut_encode_prepare_new_block(density_memory_location *restrict out, density_argonaut_encode_state *restrict state) {
    if (DENSITY_ARGONAUT_ENCODE_MINIMUM_OUTPUT_LOOKAHEAD > out->available_bytes)
        return DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT;

    switch (state->wordCount) {
        case DENSITY_ARGONAUT_PREFERRED_EFFICIENCY_CHECK_WORDS:
            if (state->efficiencyChecked ^ 0x1) {
                state->efficiencyChecked = 1;
                return DENSITY_KERNEL_ENCODE_STATE_INFO_EFFICIENCY_CHECK;
            }
            break;
        case DENSITY_ARGONAUT_PREFERRED_BLOCK_WORDS:
            state->wordCount = 0;
            state->efficiencyChecked = 0;

#if DENSITY_ENABLE_PARALLELIZABLE_DECOMPRESSIBLE_OUTPUT == DENSITY_YES
            if (state->resetCycle)
                state->resetCycle--;
            else {
                density_argonaut_dictionary_reset(&state->dictionary);
                state->resetCycle = DENSITY_DICTIONARY_PREFERRED_RESET_CYCLE - 1;
            }
#endif

            return DENSITY_KERNEL_ENCODE_STATE_INFO_NEW_BLOCK;
        default:
            break;
    }
    density_argonaut_encode_prepare_new_signature(out, state);

    return DENSITY_KERNEL_ENCODE_STATE_READY;
}

DENSITY_FORCE_INLINE void density_argonaut_encode_push_to_signature(density_memory_location *restrict out, density_argonaut_encode_state *state, uint64_t content, uint_fast8_t bits) {
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    *(state->signature) |= (content) << state->shift;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
    *(state->signature) |= (content) << ((56 - (state->shift & ~0x7)) + (state->shift & 0x7));
#endif

    state->shift += bits;

    if (state->shift >= 0x40) {
        uint8_t remainder = (uint_fast8_t) (state->shift & 0x3f);
        density_argonaut_encode_prepare_new_signature(out, state);
        density_argonaut_encode_push_to_signature(out, state, content >> (sizeof(content) - remainder), remainder);
    }
}

DENSITY_FORCE_INLINE void density_argonaut_encode_push_to_signature_dual(density_memory_location *restrict out, density_argonaut_encode_state *state, uint32_t contentA, uint_fast8_t bitsA, uint32_t contentB, uint_fast8_t bitsB) {
    density_argonaut_encode_push_to_signature(out, state, (contentA << bitsB) | contentB, bitsA + bitsB);
}

DENSITY_FORCE_INLINE uint8_t density_argonaut_encode_fetch_form_rank_for_use(density_argonaut_encode_state *state, DENSITY_ARGONAUT_FORM form) {
    uint8_t rank = state->formStatistics[form].rank;
    state->formStatistics[form].usage++;
    if (rank) if (state->formStatistics[form].usage > state->formRanks[rank - 1].statistics->usage) {
        density_argonaut_form_statistics *replaced = state->formRanks[rank - 1].statistics;
        replaced->rank++;
        state->formRanks[rank - 1].statistics = &(state->formStatistics[form]);
        state->formRanks[rank].statistics = replaced;
        state->formStatistics[form].rank--;
    }

    return rank;
}

DENSITY_FORCE_INLINE void density_argonaut_encode_process_word(density_memory_location *restrict out, density_argonaut_encode_state *state) {
    // Check word predictions
    if (state->word.length <= 4) {
        if (state->dictionary.wordPredictions[state->lastByteHash].next_word.as_uint64_t == state->word.as_uint64_t) {
            uint8_t rank = density_argonaut_encode_fetch_form_rank_for_use(state, DENSITY_ARGONAUT_FORM_PREDICTIONS);
            density_argonaut_encode_push_to_signature(out, state, 0, rank + (uint8_t) 1);
            return;
        } else
            state->dictionary.wordPredictions[state->lastByteHash].next_word.as_uint64_t = state->word.as_uint64_t;
    }

    // Word hashes
    uint16_t hash16 = (uint16_t) ((DENSITY_ARGONAUT_HASH64_MULTIPLIER * state->word.as_uint64_t) >> (64 - DENSITY_ARGONAUT_DICTIONARY_BITS));
    state->lastByteHash = (uint8_t) (hash16 ^ (hash16 >> 8));

    // Double dictionary search
    bool alternateDictionary = false;
    density_argonaut_dictionary_word_entry *wordFound = &state->dictionary.wordsA[hash16];
    if (wordFound->word.as_uint64_t != state->word.as_uint64_t) {
        wordFound = &state->dictionary.wordsB[hash16];
        alternateDictionary = true;
    }
    if (wordFound->word.as_uint64_t == state->word.as_uint64_t) {
        wordFound->usage++;
        wordFound->durability++;

        // Update usage and rank
        if (wordFound->rank) {
            uint_fast8_t lowerRank = wordFound->rank;
            uint_fast8_t upperRank = lowerRank - (uint_fast8_t) 1;
            if (state->dictionary.wordRanks[upperRank] == NULL) {
                wordFound->rank = upperRank;
                state->dictionary.wordRanks[upperRank] = wordFound;
                state->dictionary.wordRanks[lowerRank] = NULL;
            } else if (wordFound->usage > state->dictionary.wordRanks[upperRank]->usage) {
                density_argonaut_dictionary_word_entry *replaced = state->dictionary.wordRanks[upperRank];
                wordFound->rank = upperRank;
                state->dictionary.wordRanks[upperRank] = wordFound;
                replaced->rank = lowerRank;
                state->dictionary.wordRanks[lowerRank] = replaced;
            }
        }

        // Check if we have an 8 bit rank
        if (wordFound->rank < 0xff) {
            uint8_t rank = density_argonaut_encode_fetch_form_rank_for_use(state, DENSITY_ARGONAUT_FORM_RANK);
            density_argonaut_encode_push_to_signature_dual(out, state, 0, rank + (uint8_t) 1, 0, density_argonaut_huffman_codes[wordFound->rank].bitSize);
            return;
        }

        // Write dictionary hash value
        uint8_t rank = density_argonaut_encode_fetch_form_rank_for_use(state, DENSITY_ARGONAUT_FORM_DICTIONARY);
        density_argonaut_encode_push_to_signature_dual(out, state, 0, rank + (uint8_t) 1, (uint8_t) alternateDictionary, 1);
        density_argonaut_encode_push_to_output(out, hash16);
    } else {
        if (wordFound->durability)
            wordFound->durability--;
        else {
            // Remove previous word from rank table
            state->dictionary.wordRanks[wordFound->rank] = NULL;

            // Assign new word values
            wordFound->word.as_uint64_t = state->word.as_uint64_t;
            wordFound->rank = 0xff;
            wordFound->usage = 1;
            wordFound->durability = 0;

            // Swap entries if origin is the alternate dictionary
            if (alternateDictionary) {
                density_argonaut_dictionary_word_entry replaced = state->dictionary.wordsA[hash16];
                state->dictionary.wordsA[hash16] = *wordFound;
                state->dictionary.wordsB[hash16] = replaced;
            }
        }

        // Word is not in a dictionary, it has to be encoded
        uint8_t rank = density_argonaut_encode_fetch_form_rank_for_use(state, DENSITY_ARGONAUT_FORM_ENCODED);
        density_argonaut_encode_push_to_signature(out, state, 0, rank + (uint8_t) 1);

        // Look for word inner predictions
        density_argonaut_encode_push_to_signature(out, state, 0, density_argonaut_huffman_codes[state->dictionary.letters[state->word.letters[0]].rank].bitSize);
        for (int wordLetter = 1; wordLetter < state->word.length; wordLetter++) {
            if (wordLetter == 5 && state->dictionary.fourgramPredictions[(((uint32_t) (state->word.as_uint64_t & 0xFFFFFFFF)) * DENSITY_ARGONAUT_HASH32_MULTIPLIER) >> 16].next_part == (uint32_t) ((state->word.as_uint64_t & 0xFFFFFFFF00000000) >> 8)) {
                density_argonaut_encode_push_to_signature(out, state, 0x3, 2);
                goto exit_inner_predictions;
            } else if (state->dictionary.unigramPredictions[(uint8_t) state->word.letters[wordLetter - 1]].next_letter == state->word.letters[wordLetter]) {
                density_argonaut_encode_push_to_signature(out, state, 0x2, 2);
            } else if (wordLetter >= 2 && state->dictionary.bigramPredictions[((uint16_t) (state->word.letters[wordLetter - 2]) << 8) + (uint16_t) state->word.letters[wordLetter - 1]].next_letter == state->word.letters[wordLetter]) {
                density_argonaut_encode_push_to_signature(out, state, 0x1, 2);
            } else {
                density_argonaut_encode_push_to_signature_dual(out, state, 0x0, 2, 0, density_argonaut_huffman_codes[state->dictionary.letters[state->word.letters[wordLetter]].rank].bitSize);
            }
        }
        exit_inner_predictions:

        // Update word inner predictions
        for (int wordLetter = 1; wordLetter < state->word.length; wordLetter++)
            state->dictionary.unigramPredictions[(uint8_t) state->word.letters[wordLetter - 1]].next_letter = (uint8_t) state->word.letters[wordLetter];

        if (state->word.length >= 2)
            for (int wordLetter = 2; wordLetter < state->word.length; wordLetter++)
                state->dictionary.bigramPredictions[((uint16_t) (state->word.letters[wordLetter - 2]) << 8) + (uint16_t) state->word.letters[wordLetter - 1]].next_letter = (uint8_t) state->word.letters[wordLetter];

        if (state->word.length == 5)
            state->dictionary.fourgramPredictions[((uint32_t) ( state->word.as_uint64_t & 0xFFFFFFFF) * DENSITY_ARGONAUT_HASH32_MULTIPLIER) >> 16].next_part = (uint32_t) ((state->word.as_uint64_t & 0xFFFFFFFF00000000) >> 8);
    }
}

DENSITY_FORCE_INLINE void density_argonaut_encode_read_word(density_memory_location *restrict in, density_argonaut_encode_state *restrict state) {
    uint8_t letter = in->pointer[0];
    bool startWithSeparator = (letter == state->dictionary.letterRanks[0]->letter);

    while(true) {
        density_argonaut_dictionary_letter_entry *letterEntry = &state->dictionary.letters[letter];

        // Stop if the word started with the separator letter and a different letter has been read
        if (startWithSeparator && letterEntry->rank)
            goto finish;

        // Update letter usage and rank
        letterEntry->usage++;
        if (letterEntry->rank) {
            uint8_t lowerRank = letterEntry->rank;
            uint8_t upperRank = lowerRank - (uint8_t) 1;
            if (letterEntry->usage > state->dictionary.letterRanks[upperRank]->usage) {
                density_argonaut_dictionary_letter_entry *swappedLetterEntry = state->dictionary.letterRanks[upperRank];
                letterEntry->rank = upperRank;
                state->dictionary.letterRanks[upperRank] = letterEntry;
                swappedLetterEntry->rank = lowerRank;
                state->dictionary.letterRanks[lowerRank] = swappedLetterEntry;
            }
        }

        // Store and read new letter
        state->word.letters[state->word.length++] = letter;
        letter = in->pointer[state->word.length];

        // Stop if word length has reached the maximum of 8 letters
        if (state->word.length == 8)
            goto finish;

        // Stop if we found the separator and the word didn't begin with a separator
        if (!startWithSeparator && !letterEntry->rank)
            goto finish;
    }

    finish:
    in->available_bytes -= state->word.length;
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_argonaut_encode_init(density_argonaut_encode_state *state) {
    state->wordCount = 0;
    state->efficiencyChecked = 0;
    density_argonaut_dictionary_reset(&state->dictionary);

#if DENSITY_ENABLE_PARALLELIZABLE_DECOMPRESSIBLE_OUTPUT == DENSITY_YES
    state->resetCycle = DENSITY_DICTIONARY_PREFERRED_RESET_CYCLE - 1;
#endif

    state->formStatistics[DENSITY_ARGONAUT_FORM_PREDICTIONS].usage = 0;
    state->formStatistics[DENSITY_ARGONAUT_FORM_PREDICTIONS].rank = 3;
    state->formStatistics[DENSITY_ARGONAUT_FORM_RANK].usage = 0;
    state->formStatistics[DENSITY_ARGONAUT_FORM_RANK].rank = 1;
    state->formStatistics[DENSITY_ARGONAUT_FORM_DICTIONARY].usage = 0;
    state->formStatistics[DENSITY_ARGONAUT_FORM_DICTIONARY].rank = 0;
    state->formStatistics[DENSITY_ARGONAUT_FORM_ENCODED].usage = 0;
    state->formStatistics[DENSITY_ARGONAUT_FORM_ENCODED].rank = 2;

    state->formRanks[0].statistics = &state->formStatistics[DENSITY_ARGONAUT_FORM_DICTIONARY];
    state->formRanks[1].statistics = &state->formStatistics[DENSITY_ARGONAUT_FORM_RANK];
    state->formRanks[2].statistics = &state->formStatistics[DENSITY_ARGONAUT_FORM_ENCODED];
    state->formRanks[3].statistics = &state->formStatistics[DENSITY_ARGONAUT_FORM_PREDICTIONS];

    uint8_t count = 0xFF;
    do {
        state->dictionary.letters[count].letter = count;
        state->dictionary.letters[count].rank = count;
        state->dictionary.letterRanks[count] = &state->dictionary.letters[count];
    } while (count--);

    uint16_t dCount = 0xFFFF;
    do {
        state->dictionary.wordsA[dCount].word.as_uint64_t = 0;
        state->dictionary.wordsA[dCount].usage = 0;
        state->dictionary.wordsA[dCount].rank = 255;
        state->dictionary.wordsB[dCount].word.as_uint64_t = 0;
        state->dictionary.wordsB[dCount].usage = 0;
        state->dictionary.wordsB[dCount].rank = 255;
    } while (dCount--);

    count = 0xFF;
    do {
        state->dictionary.wordRanks[count] = NULL;
    } while (count--);

    return exitProcess(state, DENSITY_ARGONAUT_ENCODE_PROCESS_PREPARE_NEW_BLOCK, DENSITY_KERNEL_ENCODE_STATE_READY);
}

#define BLOCK_READ 256

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_argonaut_encode_continue(density_memory_teleport *restrict in, density_memory_location *restrict out, density_argonaut_encode_state *restrict state) {
    DENSITY_KERNEL_ENCODE_STATE returnState;
    density_memory_location *readMemoryLocation = NULL;

    // Dispatch
    switch (state->process) {
        case DENSITY_ARGONAUT_ENCODE_PROCESS_PREPARE_NEW_BLOCK:
            goto prepare_new_block;
        case DENSITY_ARGONAUT_ENCODE_PROCESS_PREPARE_INPUT:
            goto prepare_input;
        case DENSITY_ARGONAUT_PROCESS_PREPARE_OUTPUT:
            goto prepare_output;
        default:
            return DENSITY_KERNEL_ENCODE_STATE_ERROR;
    }

    // Prepare new block
    prepare_new_block:
    if ((returnState = density_argonaut_encode_prepare_new_block(out, state)))
        return exitProcess(state, DENSITY_ARGONAUT_ENCODE_PROCESS_PREPARE_NEW_BLOCK, returnState);

    // Prepare input
    prepare_input:
    if (!(readMemoryLocation = density_memory_teleport_read(in, BLOCK_READ)))
        return exitProcess(state, DENSITY_ARGONAUT_ENCODE_PROCESS_PREPARE_INPUT, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT);
    readMemoryLocation->available_bytes -= BLOCK_READ;

    // Prepare output
    prepare_output:
    if(out->available_bytes < BLOCK_READ)
        return exitProcess(state, DENSITY_ARGONAUT_ENCODE_PROCESS_PREPARE_INPUT, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT);

    // Read and process words
    density_argonaut_encode_read_and_process_word_batch(readMemoryLocation, out, state);
    density_argonaut_encode_read_and_process_word_batch(readMemoryLocation, out, state);
    density_argonaut_encode_read_and_process_word_batch(readMemoryLocation, out, state);
    density_argonaut_encode_read_and_process_word_batch(readMemoryLocation, out, state);

    /*uint8_t separator = state->dictionary.letterRanks[0]->letter;
    if (density_unlikely(state->word.length)) {
        state->word.as_uint64_t |= ((*(uint64_t *) (readMemoryLocation->pointer)) << state->word.length);
        const uint8_t addedLength = density_argonaut_encode_add_letters_until_separator_limited(in, separator, (uint_fast64_t) (DENSITY_ARGONAUT_DICTIONARY_MAX_WORD_LETTERS - state->word.length));
        state->word.length += addedLength;
    } else {
        state->word.as_uint64_t = *(uint64_t *) (in->pointer);
        if (density_likely(in->available_bytes > DENSITY_ARGONAUT_DICTIONARY_MAX_WORD_LETTERS)) {
            state->word.length = density_argonaut_encode_find_first_separator_position(&state->word.as_uint64_t, separator);
            in->pointer += state->word.length;
            in->available_bytes -= state->word.length;
        } else {
            const uint8_t remaining = (uint8_t) (readMemoryLocation->available_bytes);
            state->word.length = density_argonaut_encode_add_letters_until_separator_limited(in, separator, remaining);
            if (state->word.length == remaining) {
                state->word.as_uint64_t &= ((((uint64_t) 1) << (state->word.length << 3)) - 1);
                return DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT
            }
        }
    }*/







    uint_fast16_t count = 0;
    uint8_t letter = readMemoryLocation->pointer[count];
    bool startedWithSeparator;

    main_loop:
    startedWithSeparator = ((state->word.length ? state->word.letters[0] : letter) == state->dictionary.letterRanks[0]->letter);

    while (true) {
        density_argonaut_dictionary_letter_entry *letterEntry = &state->dictionary.letters[letter];

        // Stop if the word started with the separator letter and a different letter has been read
        if (startedWithSeparator && letterEntry->rank)
            break;

        // Update letter usage and rank
        letterEntry->usage++;
        if (letterEntry->rank) {
            uint8_t lowerRank = letterEntry->rank;
            uint8_t upperRank = lowerRank - (uint8_t) 1;
            if (letterEntry->usage > state->dictionary.letterRanks[upperRank]->usage) {
                density_argonaut_dictionary_letter_entry *swappedLetterEntry = state->dictionary.letterRanks[upperRank];
                letterEntry->rank = upperRank;
                state->dictionary.letterRanks[upperRank] = letterEntry;
                swappedLetterEntry->rank = lowerRank;
                state->dictionary.letterRanks[lowerRank] = swappedLetterEntry;
            }
        }

        state->word.letters[state->word.length++] = letter;
        if (count == BLOCK_READ - 1)
            goto read_chunk;
        letter = readMemoryLocation->pointer[++count];

        // Stop if word length has reached the maximum of 8 letters
        if (state->word.length == 8)
            break;

        // Stop if we found the separator and the word didn't begin with a separator
        if (!startedWithSeparator && !letterEntry->rank)
            break;
    }

    // Masking
    state->wordCount++;

    write_word:
    if(readMemoryLocation == NULL) {
        if (!(readMemoryLocation = density_memory_teleport_read(in, BLOCK_READ)))
            return exitProcess(state, DENSITY_ARGONAUT_PROCESS_PREPARE_OUTPUT, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_INPUT);
    }
    if (256 > out->available_bytes)
        return exitProcess(state, DENSITY_ARGONAUT_PROCESS_PREPARE_OUTPUT, DENSITY_KERNEL_ENCODE_STATE_STALL_ON_OUTPUT);
    readMemoryLocation->available_bytes -= BLOCK_READ;
    uint64_t word = state->word.as_uint64_t;
    if (state->word.length < 8)
        word &= (0xFFFFFFFFFFFFFFFFllu >> (bitsizeof(density_argonaut_word) - state->word.length * bitsizeof(uint8_t)));

    // Process word
    density_argonaut_encode_process_word(out, state, word);

    state->word.length = 0;
    goto main_loop;
}

DENSITY_FORCE_INLINE DENSITY_KERNEL_ENCODE_STATE density_argonaut_encode_finish(density_memory_teleport *restrict in, density_memory_location *restrict out, density_argonaut_encode_state *restrict state) {
    uint_fast64_t available = density_memory_teleport_available(in);
    density_memory_location *readMemoryLocation = density_memory_teleport_read(in, available);
    readMemoryLocation->available_bytes -= available;

    return DENSITY_KERNEL_ENCODE_STATE_READY;
}