/*
 * Copyright (c) 2019 UltraSoC Technologies Limited
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */


#include <assert.h>
#include <stdlib.h>
#include "decoder-algorithm-public.h"


/* optionally enable additional debugging ? */
#if !defined(DEBUG)
#   define DEBUG    0   /* turn off debug by default */
#endif  /* DEBUG */


/*
 * Extract the most-significant bit of an integer.
 */
#define MSB(x)  (((x)>>(8*(sizeof(x))-1)) & 0x1)


/*
 * Initialize the PC to a known "bad address".
 * Detect if we ever try and use this address!
 */
#define SENTINEL_BAD_ADDRESS    0xbadadd


/*
 * Fake up some values that would be obtained through
 * "discovery", or means other than "te_inst" messages.
 */
static const struct
{
    unsigned int call_counter_width;    /* 3-bits */
    unsigned int iaddress_lsb;          /* 2-bits */
} discovery_response =
{
    .call_counter_width = 7,    /* maximum of 512 calls on return_stack[] */
    .iaddress_lsb = 1,          /* 1 == compressed instructions supported */
};

static const te_support_t te_support =
{
    .full_address = 0,          /* use differential addresses */
    .implicit_return = 0,       /* disable using return_stack[] */
};


/*
 * Process an unrecoverable error with the trace-decoder's algorithm.
 * This is indicative of a serious malfunction - this should never happen!
 * This function prints a diagnostic, and it will call exit() to terminate.
 * If the parameter "instr" is not NULL, then it will also print the
 * disassembly line of the instruction ("instr") passed in.
 * NOTE: this function will never return to its caller!
 * However, any functions registered with atexit() will be called.
 */
static void unrecoverable_error(
    const te_decoded_instruction_t * const instr,
    const char * const message)
{
    assert(message);

    printf("ERROR: %s\n", message);

    if (instr)
    {
        printf("Whilst processing the following instruction:\n");
        printf("%12lx:\t%s\n", instr->decode.pc, instr->line);
    }

    exit(1);    /* do not return ... bye bye */
}


/*
 * for the address given, find the raw binary value of the instruction at
 * that address (using the external function te_get_instruction), and then use
 * the open-source riscv-disassembler library to decode, and then cache it.
 */
static te_decoded_instruction_t * get_instr(
    te_decoder_state_t * const decoder,
    const te_address_t address,
    te_decoded_instruction_t * const instr)
{
    const size_t slot = TE_SLOT_NUMBER(address);
    rv_inst instruction;
    unsigned length;

    assert(decoder);
    assert(instr);
    assert(SENTINEL_BAD_ADDRESS != address);

    decoder->num_gets++;        /* update statistics */

    /*
     * if the address matches the decoded one passed in ...
     * ... then just return it! Nothing to do this time!
     */
    if ( (instr->decode.pc == address) )
    {
        decoder->num_same++;        /* update statistics */
        return instr;       /* referenced data is unchanged */
    }

    /* is "address" currently in our decoded cache ? */
    if (decoder->decoded_cache[slot].decode.pc == address)
    {
        decoder->num_hits++;        /* update statistics */
        /* copy, and return the cached decode */
        *instr = decoder->decoded_cache[slot];
        return instr;       /* referenced data is updated */
    }

    /* otherwise, we need to do a bit of disassembly work ... */

    /* first, get the raw instruction (and its length), from its address */
    length = te_get_instruction(
        decoder->user_data,
        address,
        &instruction);

    assert( (4 == length) ||
            (2 == length) );

    /* cache the length of the instruction, for instruction_size() */
    instr->length = length;

    /*
     * Use the modified riscv-disassembler open-source library to decode
     * the instruction. This repository is available from:
     *
     * https://github.com/ultrasoc/riscv-disassembler/tree/ultrasoc
     *
     * Note: predicates in this code assumes that pseudo-instructions
     * are not lifted e.g. decode is not "ret", but "jalr x0,0(x1)".
     */
    (void)disasm_inst_adv(
        &instr->decode,
        instr->line,
        sizeof(instr->line) - 1,
        decoder->isa,
        address,
        instruction,
        false);     /* false: do not lift pseudo-instructions */

    /* save the freshly decoded instruction in the decoded cache */
    decoder->decoded_cache[slot] = *instr;

    /*
     * finally, return the pointer to te_decoded_instruction_t passed in, whose
     * referenced data will have been updated (in situ), and
     * added to the decoded_cache[] cache.
     */
    return instr;
}


/*
 * Returns the size of the instruction in bytes
 * Only safe to be called after get_instr() with instr
 */
static unsigned instruction_size(
    const te_decoded_instruction_t * const instr)
{
    assert(instr);

    return instr->length;
}


/*
 * Note, this function does not calculate nor even update the PC.
 * It is merely a single control point that should be called
 * each time the PC is updated, so we can inspect, check and record
 * each and every transition of the PC in a consistent manner.
 * This helps with checking the correctness of the decoder.
 *
 * Ultimately, the main purpose of this function is to call the external
 * function te_advance_decoded_pc() to disseminate the new value of the PC.
 */
static void disseminate_pc(
    te_decoder_state_t * const decoder)
{
    te_decoded_instruction_t instr = { .decode.pc = SENTINEL_BAD_ADDRESS };

    assert(decoder);

    if (DEBUG)
    {
        /* optionally show the transition of the PC */
        te_log_printf("    set_pc 0x%08lx -> 0x%08lx\t%lu\n",
            decoder->last_pc, decoder->pc, decoder->instruction_count);
    }

    /* do some sanity checks ... just in case! */
    assert(SENTINEL_BAD_ADDRESS != decoder->pc);
    assert(decoder->last_pc != decoder->pc);
    if (decoder->instruction_count)
    {
        /* it is NOT the first transition */
        assert(SENTINEL_BAD_ADDRESS != decoder->last_pc);
    }
    else
    {
        /* it is the FIRST transition */
        assert(SENTINEL_BAD_ADDRESS == decoder->last_pc);
    }

    /* decode & disassemble the instruction at the new PC */
    (void)get_instr(decoder, decoder->pc, &instr);

    if (DEBUG)
    {
        /* optionally show the transition & instruction at the new PC */
        te_log_printf("%s\t%8lx -> %8lx:\t%s\n",
            (decoder->pc == decoder->address) ? "---->" : "",
            decoder->last_pc,
            decoder->pc,
            instr.line);
    }

    /* notify the user that the PC has been updated */
    te_advance_decoded_pc(
        decoder->user_data,
        decoder->last_pc,
        decoder->pc,
        &instr);

    /* advance the count of PC transitions */
    decoder->instruction_count++;
}


/*
 * Determine if current instruction is a branch
 */
static bool is_branch(
    const te_decoded_instruction_t * const instr)
{
    bool predicate = false;

    assert(instr);

    if ( (instr->decode.op == rv_op_beq)    ||
         (instr->decode.op == rv_op_bne)    ||
         (instr->decode.op == rv_op_blt)    ||
         (instr->decode.op == rv_op_bge)    ||
         (instr->decode.op == rv_op_bltu)   ||
         (instr->decode.op == rv_op_bgeu)   ||
         (instr->decode.op == rv_op_c_beqz) ||
         (instr->decode.op == rv_op_c_bnez) )
    {
        predicate = true;
    }

    return predicate;
}


/*
 * Determine if current instruction is a branch, adjust the branch
 * count/map, and return the "taken" status
 */
static bool is_taken_branch(
    te_decoder_state_t * const decoder,
    const te_decoded_instruction_t * const instr)
{
    bool taken = false;     /* assume branch not taken */

    assert(decoder);
    assert(instr);

    if (!is_branch(instr))
    {
        return false;
    }

    if (0 == decoder->branches)
    {
        unrecoverable_error(instr,
            "cannot resolve branch (branch-map depleted)!");
    }
    else
    {
        taken = !(decoder->branch_map & 1);  /* bit [0] */
        decoder->branches--;
        decoder->branch_map >>= 1;   /* right-shift one bit */
    }

    return taken;
}


/*
 * Determine if instruction is an inferrable jump
 */
static bool is_inferrable_jump(
    const te_decoded_instruction_t * const instr)
{
    bool predicate = false;

    assert(instr);

    if ( (instr->decode.op == rv_op_jal)    ||
         (instr->decode.op == rv_op_c_jal)  ||
         (instr->decode.op == rv_op_c_j)    ||
         ( (instr->decode.op == rv_op_jalr) &&
           (0 == instr->decode.rs1) ) )
    {
        predicate = true;
    }

    return predicate;
}


/*
 * Determine if instruction is an uninferrable jump
 */
static bool is_uninferrable_jump(
    const te_decoded_instruction_t * const instr)
{
    bool predicate = false;

    assert(instr);

    if ( ( (instr->decode.op == rv_op_jalr) &&
           (0 != instr->decode.rs1) )       ||
         (instr->decode.op == rv_op_c_jalr) ||
         (instr->decode.op == rv_op_c_jr) )
    {
        predicate = true;
    }

    return predicate;
}


/*
 * Determine if instruction is an uninferrable discontinuity
 */
static bool is_uninferrable_discon(
    const te_decoded_instruction_t * const instr)
{
    bool predicate = false;

    assert(instr);

    /*
     * Note: The exception reporting mechanism means it is not necessary
     * to include ECALL, EBREAK or C.EBREAK in this predicate
     */
    if ( is_uninferrable_jump(instr)        ||
         (instr->decode.op == rv_op_uret)   ||
         (instr->decode.op == rv_op_sret)   ||
         (instr->decode.op == rv_op_mret)   ||
         (instr->decode.op == rv_op_dret) )
    {
        predicate = true;
    }

    return predicate;
}


/*
 * Determine if instruction is a sequentially inferrable jump
 */
static bool is_sequential_jump(
    te_decoder_state_t * const decoder,
    const te_decoded_instruction_t * const instr,
    const te_address_t prev_addr)
{
    te_decoded_instruction_t prev_instr = { .decode.pc = SENTINEL_BAD_ADDRESS };
    bool predicate = false;

    assert(decoder);
    assert(instr);

    if (!is_uninferrable_jump(instr))
    {
        return false;
    }

    (void)get_instr(decoder, prev_addr, &prev_instr);

    if ( (prev_instr.decode.op == rv_op_auipc) ||
         (prev_instr.decode.op == rv_op_lui)   ||
         (prev_instr.decode.op == rv_op_c_lui) )
    {
        predicate = (instr->decode.rs1 == prev_instr.decode.rd);
    }

    return predicate;
}


/*
 * Find the target of a sequentially inferrable jump
 */
static te_address_t sequential_jump_target(
    te_decoder_state_t * const decoder,
    const te_address_t addr,
    const te_address_t prev_addr)
{
    te_decoded_instruction_t instr      = { .decode.pc = SENTINEL_BAD_ADDRESS };
    te_decoded_instruction_t prev_instr = { .decode.pc = SENTINEL_BAD_ADDRESS };
    te_address_t target = 0;

    assert(decoder);

    (void)get_instr(decoder, addr, &instr);
    (void)get_instr(decoder, prev_addr, &prev_instr);

    if (prev_instr.decode.op == rv_op_auipc)
    {
        target = prev_addr;
    }

    target += prev_instr.decode.imm;

    if (instr.decode.op == rv_op_jalr)
    {
        target += instr.decode.imm;
    }

    return target;
}


/*
 * Determine if instruction is a call
 * - excludes tail calls as they do not push an address onto the return stack
 */
static bool is_call(
    const te_decoded_instruction_t * const instr)
{
    bool predicate = false;

    assert(instr);

    if ( ( (instr->decode.op == rv_op_jalr) &&
           (1 == instr->decode.rd) )        ||
         (instr->decode.op == rv_op_c_jalr) ||
         ( (instr->decode.op == rv_op_jal)  &&
           (1 == instr->decode.rd) )        ||
         (instr->decode.op == rv_op_c_jal) )
    {
        predicate = true;
    }

    return predicate;
}


/*
 * Determine if instruction return address can be implicitly inferred
 */
static bool is_implicit_return(
    const te_decoder_state_t * const decoder,
    const te_decoded_instruction_t * const instr)
{
    bool predicate = false;
    assert(decoder);
    assert(instr);

    if (te_support.implicit_return == 0)
    {
        return false;   /* Implicit return mode is disabled */
    }

    if ( ( (instr->decode.op == rv_op_jalr) &&
           (1 == instr->decode.rs1)         &&
           (0 == instr->decode.rd) )        ||
         ( (instr->decode.op == rv_op_c_jr) &&
           (1 == instr->decode.rs1) ) )
    {
        predicate = (decoder->call_counter > 0);
    }

    return predicate;
}


/*
 * Push address onto return stack
 */
static void push_return_stack(
    te_decoder_state_t * const decoder,
    const te_address_t address)
{
    const size_t call_counter_max = 1u << (discovery_response.call_counter_width + 2);
    te_decoded_instruction_t instr = { .decode.pc = SENTINEL_BAD_ADDRESS };
    te_address_t link = address;
    size_t i;

    assert(decoder);
    assert(decoder->call_counter <= call_counter_max);
    assert(call_counter_max <= TE_MAX_CALL_DEPTH);

    if (call_counter_max == decoder->call_counter)
    {
        /* Delete oldest entry from stack to make room for new entry added below */
        decoder->call_counter--;
        for (i = 0; i < decoder->call_counter; i++)
        {
            decoder->return_stack[i] = decoder->return_stack[i+1];
        }
    }

    /* link register is address of next spatial instruction */
    (void)get_instr(decoder, address, &instr);
    link += instruction_size(&instr);

    /* push link register to top of the stack */
    decoder->return_stack[decoder->call_counter] = link;
    decoder->call_counter++;
}


/*
 * Pop address from return stack
 */
static te_address_t pop_return_stack(
    te_decoder_state_t * const decoder)
{
    assert(decoder);

    const te_address_t link = decoder->return_stack[decoder->call_counter];

    /*
     * Note: this function is not called if call_counter is 0,
     * so no need to check for underflow
     */
    decoder->call_counter--;

    return link;
}


/*
 * Compute the next PC
 */
static void next_pc(
    te_decoder_state_t * const decoder,
    const te_address_t address)
{
    assert(decoder);

    const te_address_t this_pc = decoder->pc;
    te_decoded_instruction_t instr = { .decode.pc = SENTINEL_BAD_ADDRESS };

    (void)get_instr(decoder, decoder->pc, &instr);

    if (is_inferrable_jump(&instr))
    {
        decoder->pc += instr.decode.imm;
    }
    else if (is_sequential_jump(decoder, &instr, decoder->last_pc))
    {
        /* lui/auipc followed by jump using same register */
        decoder->pc = sequential_jump_target(decoder, decoder->pc, decoder->last_pc);
    }
    else if (is_implicit_return(decoder, &instr))
    {
        decoder->pc = pop_return_stack(decoder);
    }
    else if (is_uninferrable_discon(&instr))
    {
        if (decoder->stop_at_last_branch)
        {
            unrecoverable_error(&instr,
                "unexpected uninferrable discontinuity");
        }
        else
        {
          decoder->pc = address;
        }
    }
    else if (is_taken_branch(decoder, &instr))
    {
        decoder->pc += instr.decode.imm;
    }
    else
    {
        decoder->pc += instruction_size(&instr);
    }

    if (is_call(&instr))
    {
        push_return_stack(decoder, this_pc);
    }

    decoder->last_pc = this_pc;
    disseminate_pc(decoder);
}


/*
 * Follow execution path to reported address
 */
static void follow_execution_path(
    te_decoder_state_t * const decoder,
    const te_address_t address,
    const te_inst_t * const te_inst)
{
    assert(decoder);

    te_address_t previous_address = decoder->pc;
    te_decoded_instruction_t instr      = { .decode.pc = SENTINEL_BAD_ADDRESS };
    te_decoded_instruction_t last_instr = { .decode.pc = SENTINEL_BAD_ADDRESS };

    assert(te_inst);
    assert( (!decoder->stop_at_last_branch)  ||
            (1==decoder->branches)           ||
            (2==decoder->branches)           ||
            (31==decoder->branches)          ||
            (32==decoder->branches) );

    (void)get_instr(decoder, decoder->pc, &instr);

    if (DEBUG)
    {
        te_log_printf("entered %s() with format = %u, pc = 0x%lx, and address = 0x%lx\n",
            __func__, te_inst->format, decoder->pc, address);
    }

    while (true)
    {
        if ( (decoder->stop_at_last_branch) &&
             (0 == decoder->branches) )
        {
            unrecoverable_error(&instr,
                "follow_execution_path() has stop_at_last_branch=true and branches=0");
        }

        if (decoder->inferred_address)
        {
            /*
             * iterate again from previously reported address to find second occurrence
             */
            next_pc(decoder, previous_address);
            (void)get_instr(decoder, decoder->pc, &instr);
            if (decoder->pc == previous_address)
            {
                decoder->inferred_address = false;
            }
        }
        else
        {
            next_pc(decoder, address);
            (void)get_instr(decoder, decoder->pc, &instr);
            if ( (1 == decoder->branches)                             &&
                 (is_branch(get_instr(decoder, decoder->pc, &instr))) &&
                 (decoder->stop_at_last_branch) )
            {
                /*
                 * Reached final branch - stop here (do not follow to next instruction
                 * as we do not yet know whether it retires)
                 */
                decoder->stop_at_last_branch = false;
                return;
            }
            if ( (decoder->pc == address) &&
                 is_uninferrable_discon(get_instr(decoder, decoder->last_pc, &last_instr)) )
            {
                /*
                 * Reached reported address following an uninferrable discontinuity - stop here
                 */
                if (decoder->branches > (is_branch(get_instr(decoder, decoder->pc, &instr)) ? 1 : 0))
                {
                    /*
                     * Check all branches processed (except 1 if this instruction is a branch)
                     */
                    unrecoverable_error(&instr,
                        "unprocessed branches");
                }
                return;
            }
            if ( (3 != te_inst->format)                         &&
                 (decoder->pc == address)                       &&
                 (te_inst->updiscon == MSB(te_inst->address))   &&
                 (decoder->branches == (is_branch(get_instr(decoder, decoder->pc, &instr)) ? 1 : 0)) )
            {
                /*
                 * All branches processed, and reached reported address, but not as an
                 * uninferrable jump target. Stop here for now, though flag indicates
                 * this may not be final retired instruction
                 */
                decoder->inferred_address = true;
                return;
            }
            if ( (3 == te_inst->format)     &&
                 (decoder->pc == address)   &&
                 (decoder->branches == (is_branch(get_instr(decoder, decoder->pc, &instr)) ? 1 : 0)) )
            {
                /* All branches processed, and reached reported address */
                return;
            }
        }
    }
}


/*
 * Process a single te_inst message.
 * Called each time a te_inst message is received.
 */
extern void te_process_te_inst(
    te_decoder_state_t * const decoder,
    const te_inst_t * const te_inst)
{
    te_decoded_instruction_t instr = { .decode.pc = SENTINEL_BAD_ADDRESS };

    assert(decoder);
    assert(te_inst);

    if (3 == te_inst->format)
    {
        decoder->inferred_address = false;
        decoder->address = (te_inst->address << discovery_response.iaddress_lsb);

        if ( (1 == te_inst->subformat) ||
             (decoder->start_of_trace) )
        {
            /* expunge any pending branches */
            decoder->branches   = 0;
            decoder->branch_map = 0;
        }
        if (is_branch(get_instr(decoder, decoder->address, &instr)))
        {
            /* 1 unprocessed branch if this instruction is a branch */
            decoder->branch_map |= te_inst->branch << decoder->branches;
            decoder->branches++;
        }

        if ( (0 == te_inst->subformat) &&
             (!decoder->start_of_trace) )
        {
            follow_execution_path(decoder, decoder->address, te_inst);
        }
        else
        {
            /*
             * Firstly, update "last_pc" to be the current PC.
             * This is essentially so that the diagnostics emitted from disseminate_pc() looks right!
             * After we return from disseminate_pc(), we will update it again!
             */
            decoder->last_pc = decoder->pc;
            decoder->pc = decoder->address;
            disseminate_pc(decoder);
            /*
             * To avoid the (unlikely, but not impossible) possibility that the
             * instructions currently at "last_pc" and "pc" happen to satisfy
             * the constraints in is_sequential_jump(), we need to guarantee
             * that does not happen, when we next call follow_execution_path().
             * Thus we update "last_pc" to a "spurious" value ... that is a
             * value which will always cause is_sequential_jump() to be false.
             * We choose "pc" as such a spurious value to write to "last_pc".
             * Thus the predicate is_sequential_jump(pc,pc) will never be true.
             * Ensure is_sequential_jump() deterministically returns
             * false immediately after the first format 3 message,
             * even though the previous PC is not known.
             */
            decoder->last_pc = decoder->pc;
        }
        decoder->start_of_trace = false;
        decoder->call_counter = 0;
    }
    else
    {
        if (decoder->start_of_trace)
        {
            /* This should not be possible! */
            unrecoverable_error(NULL,
                "Expecting trace to start with a format 3 message");
        }
        if ( (2 == te_inst->format) ||
             (0 != te_inst->branches) )
        {
            decoder->stop_at_last_branch = false;
            if (te_support.full_address)
            {
                decoder->address  = (te_inst->address << discovery_response.iaddress_lsb);
            }
            else
            {
                decoder->address += (te_inst->address << discovery_response.iaddress_lsb);
            }
        }
        if (1 == te_inst->format)
        {
            decoder->stop_at_last_branch = (te_inst->branches == 0);
            /*
             * Branch map will contain <= 1 branch
             * (1 if last reported instruction was a branch)
             */
            assert(decoder->branches <= 1);
            assert( ( (0==decoder->branches) && (0==decoder->branch_map) ) ||
                    ( (1==decoder->branches) && (0==(decoder->branch_map&(~1))) ) );
            decoder->branch_map |= te_inst->branch_map << decoder->branches;
            if (0 == te_inst->branches)
            {
                decoder->branches += 31;
            }
            else
            {
                decoder->branches += te_inst->branches;
            }
        }
        follow_execution_path(decoder, decoder->address, te_inst);
    }
}


/*
 * Process a single te_support message.
 * Called each time a te_support message is received.
 */
extern void te_process_te_support(
    te_decoder_state_t * const decoder,
    const te_support_t * const te_support)
{
    assert(decoder);
    assert(te_support);

    if (0 == te_support->support_type)
    {
        if ( (QUAL_STATUS_ENDED_NTR == te_support->qual_status) ||
             (QUAL_STATUS_ENDED_REP == te_support->qual_status) )
        {
            /* Trace ended, so get ready to start again */
            decoder->start_of_trace = true;
        }
        if ( (QUAL_STATUS_ENDED_NTR == te_support->qual_status) &&
             (decoder->inferred_address) )
        {
            const te_address_t previous_address = decoder->pc;
            decoder->inferred_address = false;
            while (true)
            {
                next_pc(decoder, previous_address);
                if (decoder->pc == previous_address)
                {
                    return;
                }
            }
        }
    }
}


/*
 * Initialize a new instance of a trace-decoder (the state for one instance).
 * If "decoder" is NULL on entry, then memory will be dynamically
 * allocated, otherwise it must point to a pre-allocated region large enough.
 * This returns a pointer to the internal "state" of the trace-decoder.
 *
 * If this function allocated memory (decoder==NULL on entry), the memory
 * should be released (by calling free()), when the instance of the
 * trace-decoder is no longer required.
 */
extern te_decoder_state_t * te_open_trace_decoder(
    te_decoder_state_t * decoder,
    void * const user_data,
    const rv_isa isa)
{
    if (decoder)
    {
        /* use provided memory, but zero it for ONE trace-decoder instance */
        memset(decoder, 0, sizeof(te_decoder_state_t));
    }
    else
    {
        /* allocate (and zero) memory for ONE trace-decoder instance */
        decoder = calloc(1, sizeof(te_decoder_state_t));
        assert(decoder);
    }

    /* bind the "user-data" to the allocated memory */
    decoder->user_data = user_data;
    decoder->isa = isa;

    /*
     * initialize some of the fields, as per the pseudo-code.
     * no need to re-initialize anything that should be zero/false!
     */
    decoder->pc = SENTINEL_BAD_ADDRESS;
    decoder->last_pc = SENTINEL_BAD_ADDRESS;
    decoder->address = SENTINEL_BAD_ADDRESS;
    decoder->start_of_trace = true;

    return decoder;
}


/*
 * if we have any yet, print out the decoded cache statistics
 */
extern void te_print_decoded_cache_statistics(
    const te_decoder_state_t * const decoder)
{
    assert(decoder);

    const float same = (float)(decoder->num_same)*100.0/(float)decoder->num_gets;
    const float hits = (float)(decoder->num_hits)*100.0/(float)decoder->num_gets;

    if (decoder->num_gets)  /* ensure we do not divide by zero */
    {
        printf("decoded-cache: same = %7lu (%5.2f%%),  hits = %8lu (%5.2f%%),"
            "total = %8lu,  combined hit-rate = %.2f%%\n",
            decoder->num_same, same,
            decoder->num_hits, hits,
            decoder->num_gets,
            same + hits);
    }
}
