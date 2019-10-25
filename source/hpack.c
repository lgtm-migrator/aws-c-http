/*
 * Copyright 2010-2018 Amazon.com, Inc. or its affiliates. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License").
 * You may not use this file except in compliance with the License.
 * A copy of the License is located at
 *
 *  http://aws.amazon.com/apache2.0
 *
 * or in the "license" file accompanying this file. This file is distributed
 * on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either
 * express or implied. See the License for the specific language governing
 * permissions and limitations under the License.
 */

#include <aws/http/private/hpack.h>

#include <aws/http/request_response.h>

#include <aws/compression/huffman.h>

#include <aws/common/byte_buf.h>
#include <aws/common/hash_table.h>
#include <aws/common/string.h>

struct aws_huffman_symbol_coder *hpack_get_coder(void);

size_t aws_hpack_get_encoded_length_integer(uint64_t integer, uint8_t prefix_size) {
    const uint8_t cut_bits = 8 - prefix_size;
    const uint8_t prefix_mask = UINT8_MAX >> cut_bits;

    if (integer < prefix_mask) {
        /* If the integer fits inside the specified number of bits but won't be all 1's, then that's all she wrote */

        return 1;
    } else {
        integer -= prefix_mask;

        size_t num_bytes = 1;
        while (integer) {
            ++num_bytes;
            integer >>= 7;
        }
        return num_bytes;
    }
}

int aws_hpack_encode_integer(uint64_t integer, uint8_t prefix_size, struct aws_byte_buf *output) {
    AWS_ASSERT(prefix_size <= 8);

    const struct aws_byte_buf output_backup = *output;

    if (output->len == output->capacity) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }
    const uint8_t cut_bits = 8 - prefix_size;
    const uint8_t prefix_mask = UINT8_MAX >> cut_bits;

    if (integer < prefix_mask) {
        /* If the integer fits inside the specified number of bits but won't be all 1's, just write it */

        /* Just write out the bits we care about */
        output->buffer[output->len] = (output->buffer[output->len] & ~prefix_mask) | (uint8_t)integer;
        ++output->len;
    } else {
        /* Set all of the bits in the first octet to 1 */
        output->buffer[output->len] = (output->buffer[output->len] & ~prefix_mask) | prefix_mask;
        ++output->len;

        integer -= prefix_mask;

        const uint64_t hi_57bit_mask = UINT64_MAX - (UINT8_MAX >> 1);

        while (integer) {
            if (output->len == output->capacity) {
                *output = output_backup;
                return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
            }

            /* Take top 7 bits from the integer */
            uint8_t this_octet = integer % 128;
            if (integer & hi_57bit_mask) {
                /* If there's more after this octet, set the hi bit */
                this_octet += 128;
            }

            aws_byte_buf_write_u8(output, this_octet);

            /* Remove the written bits */
            integer >>= 7;
        }
    }

    return AWS_OP_SUCCESS;
}

struct aws_http_header s_static_header_table[] = {
#define HEADER(_index, _name)                                                                                          \
    [_index] = {                                                                                                       \
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_name),                                                          \
    },

#define HEADER_WITH_VALUE(_index, _name, _value)                                                                       \
    [_index] = {                                                                                                       \
        .name = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_name),                                                          \
        .value = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_value),                                                        \
    },

#include <aws/http/private/hpack_header_static_table.def>

#undef HEADER
#undef HEADER_WITH_VALUE
};
static const size_t s_static_header_table_size = AWS_ARRAY_SIZE(s_static_header_table);

struct aws_byte_cursor s_static_header_table_name_only[] = {
#define HEADER(_index, _name) [_index] = AWS_BYTE_CUR_INIT_FROM_STRING_LITERAL(_name),
#define HEADER_WITH_VALUE(_index, _name, _value) HEADER(_index, _name)

#include <aws/http/private/hpack_header_static_table.def>

#undef HEADER
#undef HEADER_WITH_VALUE
};

/* aws_http_header * -> size_t */
static struct aws_hash_table s_static_header_reverse_lookup;
/* aws_byte_cursor * -> size_t */
static struct aws_hash_table s_static_header_reverse_lookup_name_only;

static uint64_t s_header_hash(const void *key) {
    const struct aws_http_header *header = key;

    return aws_hash_byte_cursor_ptr(&header->name);
}

static bool s_header_eq(const void *a, const void *b) {
    const struct aws_http_header *left = a;
    const struct aws_http_header *right = b;

    if (!aws_byte_cursor_eq(&left->name, &right->name)) {
        return false;
    }

    /* If the header stored in the table doesn't have a value, then it's a match */
    return !right->value.ptr || aws_byte_cursor_eq(&left->value, &right->value);
}

void aws_hpack_static_table_init(struct aws_allocator *allocator) {

    int result = aws_hash_table_init(
        &s_static_header_reverse_lookup,
        allocator,
        s_static_header_table_size - 1,
        s_header_hash,
        s_header_eq,
        NULL,
        NULL);
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);

    result = aws_hash_table_init(
        &s_static_header_reverse_lookup_name_only,
        allocator,
        s_static_header_table_size - 1,
        aws_hash_byte_cursor_ptr,
        (aws_hash_callback_eq_fn *)aws_byte_cursor_eq,
        NULL,
        NULL);
    AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);

    /* Process in reverse so that name_only prefers lower indices */
    for (size_t i = s_static_header_table_size; i > 0; --i) {
        /* Thanks, 1-based indexing. Thanks. */
        const size_t static_index = i - 1;

        result = aws_hash_table_put(
            &s_static_header_reverse_lookup, &s_static_header_table[static_index], (void *)static_index, NULL);
        AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);

        result = aws_hash_table_put(
            &s_static_header_reverse_lookup_name_only,
            &s_static_header_table_name_only[static_index],
            (void *)(static_index),
            NULL);
        AWS_FATAL_ASSERT(AWS_OP_SUCCESS == result);
    }
}

void aws_hpack_static_table_clean_up() {
    aws_hash_table_clean_up(&s_static_header_reverse_lookup);
    aws_hash_table_clean_up(&s_static_header_reverse_lookup_name_only);
}

/* Insertion is backwards, indexing is forwards */
struct aws_hpack_context {
    struct aws_allocator *allocator;

    struct aws_huffman_encoder encoder;
    struct aws_huffman_decoder decoder;

    struct {
        struct aws_http_header *buffer;
        size_t max_elements;
        size_t num_elements;
        size_t index_0;

        /* aws_http_header * -> size_t */
        struct aws_hash_table reverse_lookup;
        /* aws_byte_cursor * -> size_t */
        struct aws_hash_table reverse_lookup_name_only;
    } dynamic_table;

    /* PRO TIP: Don't union these, since string_decode calls integer_decode */
    struct hpack_progress_integer {
        enum {
            HPACK_INTEGER_STATE_INIT,
            HPACK_INTEGER_STATE_VALUE,
        } state;
        uint8_t bit_count;
    } progress_integer;
    struct hpack_progress_string {
        enum {
            HPACK_STRING_STATE_INIT,
            HPACK_STRING_STATE_LENGTH,
            HPACK_STRING_STATE_VALUE,
        } state;
        bool use_huffman;
        uint64_t length;
    } progress_string;
};

struct aws_hpack_context *aws_hpack_context_new(struct aws_allocator *allocator, size_t max_dynamic_elements) {

    struct aws_hpack_context *context = aws_mem_acquire(allocator, sizeof(struct aws_hpack_context));
    if (!context) {
        return NULL;
    }
    AWS_ZERO_STRUCT(*context);
    context->allocator = allocator;

    /* Initialize the huffman coders */
    struct aws_huffman_symbol_coder *hpack_coder = hpack_get_coder();
    aws_huffman_encoder_init(&context->encoder, hpack_coder);
    aws_huffman_decoder_init(&context->decoder, hpack_coder);

    /* Initialize dynamic table */
    if (max_dynamic_elements) {
        context->dynamic_table.buffer = aws_mem_calloc(allocator, max_dynamic_elements, sizeof(struct aws_http_header));
        if (!context->dynamic_table.buffer) {
            goto dynamic_table_buffer_failed;
        }
    }
    context->dynamic_table.max_elements = max_dynamic_elements;
    context->dynamic_table.num_elements = 0;
    context->dynamic_table.index_0 = 0;

    if (aws_hash_table_init(
            &context->dynamic_table.reverse_lookup,
            allocator,
            max_dynamic_elements,
            s_header_hash,
            s_header_eq,
            NULL,
            NULL)) {
        goto reverse_lookup_failed;
    }

    if (aws_hash_table_init(
            &context->dynamic_table.reverse_lookup_name_only,
            allocator,
            max_dynamic_elements,
            aws_hash_byte_cursor_ptr,
            (aws_hash_callback_eq_fn *)aws_byte_cursor_eq,
            NULL,
            NULL)) {
        goto name_only_failed;
    }

    return context;

name_only_failed:
    aws_hash_table_clean_up(&context->dynamic_table.reverse_lookup);

reverse_lookup_failed:
    if (context->dynamic_table.buffer) {
        aws_mem_release(allocator, context->dynamic_table.buffer);
    }

dynamic_table_buffer_failed:
    aws_mem_release(allocator, context);

    return NULL;
}

void aws_hpack_context_destroy(struct aws_hpack_context *context) {
    if (!context) {
        return;
    }
    if (context->dynamic_table.buffer) {
        aws_mem_release(context->allocator, context->dynamic_table.buffer);
    }
    aws_hash_table_clean_up(&context->dynamic_table.reverse_lookup);
    aws_hash_table_clean_up(&context->dynamic_table.reverse_lookup_name_only);
    aws_mem_release(context->allocator, context);
}

void aws_hpack_context_reset_decode(struct aws_hpack_context *context) {
    AWS_PRECONDITION(context);

    AWS_ZERO_STRUCT(context->progress_integer);
    AWS_ZERO_STRUCT(context->progress_string);
}

const struct aws_http_header *aws_hpack_get_header(const struct aws_hpack_context *context, size_t index) {
    if (index == 0 || index >= s_static_header_table_size + context->dynamic_table.num_elements) {
        aws_raise_error(AWS_ERROR_INVALID_INDEX);
        return NULL;
    }

    /* Check static table */
    if (index < s_static_header_table_size) {
        return &s_static_header_table[index];
    }

    /* Check dynamic table */
    index -= s_static_header_table_size;
    AWS_ASSERT(index < context->dynamic_table.num_elements);
    return &context->dynamic_table
                .buffer[(context->dynamic_table.index_0 + index) % context->dynamic_table.max_elements];
}

size_t aws_hpack_find_index(
    const struct aws_hpack_context *context,
    const struct aws_http_header *header,
    bool *found_value) {

    *found_value = false;

    /* Check static table */
    struct aws_hash_element *elem = NULL;
    aws_hash_table_find(&s_static_header_reverse_lookup, header, &elem);
    if (elem) {
        *found_value = ((const struct aws_http_header *)elem->key)->value.len;
        return (size_t)elem->value;
    }
    /* If not found, check name only table. Don't set found_value, it will be false */
    aws_hash_table_find(&s_static_header_reverse_lookup_name_only, &header->name, &elem);
    if (elem) {
        return (size_t)elem->value;
    }

    /* Check dynamic table */
    aws_hash_table_find(&context->dynamic_table.reverse_lookup, header, &elem);
    if (elem) {
        /* If an element was found, check if it has a value */
        *found_value = ((const struct aws_http_header *)elem->key)->value.len;
    } else {
        /* If not found, check name only table. Don't set found_value, it will be false */
        aws_hash_table_find(&context->dynamic_table.reverse_lookup_name_only, &header->name, &elem);
    }

    if (elem) {
        size_t index;
        const size_t absolute_index = (size_t)elem->value;
        if (absolute_index >= context->dynamic_table.index_0) {
            index = absolute_index - context->dynamic_table.index_0;
        } else {
            index = (context->dynamic_table.max_elements - context->dynamic_table.index_0) + absolute_index;
        }
        /* Need to add the static table size to re-base indicies */
        index += s_static_header_table_size;
        return index;
    }

    return 0;
}

int aws_hpack_insert_header(struct aws_hpack_context *context, const struct aws_http_header *header) {

    /* Don't move forward if no elements allowed in the dynamic table */
    if (AWS_UNLIKELY(context->dynamic_table.max_elements == 0)) {
        return AWS_OP_SUCCESS;
    }

    /* Cache state */
    const size_t old_index_0 = context->dynamic_table.index_0;
    bool removed_from_name_table = false;

    /* Decrement index 0, wrapping if necessary */
    if (context->dynamic_table.index_0 == 0) {
        context->dynamic_table.index_0 = context->dynamic_table.max_elements - 1;
    } else {
        context->dynamic_table.index_0--;
    }
    struct aws_http_header *table_header = &context->dynamic_table.buffer[context->dynamic_table.index_0];

    /* If max size reached, start rotating out headers */
    if (context->dynamic_table.num_elements == context->dynamic_table.max_elements) {
        /* Remove old header from hash tables */
        if (aws_hash_table_remove(&context->dynamic_table.reverse_lookup, table_header, NULL, NULL)) {
            goto error;
        }

        /* If the name-only lookup is pointing to the element we're removing, it needs to go.
         * If not, it's pointing to a younger, sexier element. */
        struct aws_hash_element *elem = NULL;
        aws_hash_table_find(&context->dynamic_table.reverse_lookup_name_only, &table_header->name, &elem);
        if (elem && elem->key == table_header) {
            if (aws_hash_table_remove_element(&context->dynamic_table.reverse_lookup_name_only, elem)) {
                goto error;
            }
            removed_from_name_table = true;
        }
    }

    /* Write the new header */
    struct aws_http_header old_header = *table_header;
    *table_header = *header;
    if (aws_hash_table_put(
            &context->dynamic_table.reverse_lookup, table_header, (void *)context->dynamic_table.index_0, NULL)) {
        /* Roll back and handle the error */
        *table_header = old_header;
        goto error;
    }
    /* Note that we can just blindly put here, we want to overwrite any older entry so it isn't accidentally removed. */
    if (aws_hash_table_put(
            &context->dynamic_table.reverse_lookup_name_only,
            &table_header->name,
            (void *)context->dynamic_table.index_0,
            NULL)) {
        /* Roll back and handle the error */
        aws_hash_table_remove(&context->dynamic_table.reverse_lookup, table_header, NULL, NULL);
        *table_header = old_header;
        goto error;
    }

    /* Increment num_elements if necessary */
    if (context->dynamic_table.num_elements < context->dynamic_table.max_elements) {
        ++context->dynamic_table.num_elements;
    }

    return AWS_OP_SUCCESS;

error:
    /* Attempt to replace old header in map */
    aws_hash_table_put(
        &context->dynamic_table.reverse_lookup, table_header, (void *)context->dynamic_table.index_0, NULL);
    if (removed_from_name_table) {
        aws_hash_table_put(
            &context->dynamic_table.reverse_lookup_name_only,
            &table_header->name,
            (void *)context->dynamic_table.index_0,
            NULL);
    }
    /* Reset index 0 */
    context->dynamic_table.index_0 = old_index_0;

    return AWS_OP_ERR;
}

int aws_hpack_resize_dynamic_table(struct aws_hpack_context *context, size_t new_max_elements) {

    /* Clear the old hash tables */
    aws_hash_table_clear(&context->dynamic_table.reverse_lookup);
    aws_hash_table_clear(&context->dynamic_table.reverse_lookup_name_only);

    struct aws_http_header *new_buffer = NULL;

    if (AWS_UNLIKELY(!new_max_elements)) {
        /* If new buffer is of size 0, don't both initializing, just clean up the old one. */
        goto cleanup_old_buffer;
    }

    new_buffer = aws_mem_calloc(context->allocator, new_max_elements, sizeof(struct aws_http_header));
    if (!new_buffer) {
        return AWS_OP_ERR;
    }

    /* Don't bother copying data if old buffer was of size 0 */
    if (AWS_UNLIKELY(context->dynamic_table.max_elements == 0)) {
        goto reset_dyn_table_state;
    }

    /* Copy as much the above block as possible */
    size_t above_block_size = context->dynamic_table.max_elements - context->dynamic_table.index_0;
    if (above_block_size > new_max_elements) {
        above_block_size = new_max_elements;
    }
    memcpy(
        new_buffer,
        context->dynamic_table.buffer + context->dynamic_table.index_0,
        above_block_size * sizeof(struct aws_http_header));

    /* Copy as much of below block as possible */
    const size_t free_blocks_available = new_max_elements - above_block_size;
    const size_t old_blocks_to_copy = context->dynamic_table.max_elements - above_block_size;
    const size_t below_block_size =
        free_blocks_available > old_blocks_to_copy ? old_blocks_to_copy : free_blocks_available;
    if (below_block_size) {
        memcpy(
            new_buffer + above_block_size,
            context->dynamic_table.buffer,
            below_block_size * sizeof(struct aws_http_header));
    }

    /* Free the old memory */
cleanup_old_buffer:
    aws_mem_release(context->allocator, context->dynamic_table.buffer);

    /* Reset state */
reset_dyn_table_state:
    if (context->dynamic_table.num_elements > new_max_elements) {
        context->dynamic_table.num_elements = new_max_elements;
    }
    context->dynamic_table.max_elements = new_max_elements;
    context->dynamic_table.index_0 = 0;
    context->dynamic_table.buffer = new_buffer;

    /* Re-insert all of the reverse lookup elements */
    for (size_t i = 0; i < context->dynamic_table.num_elements; ++i) {
        aws_hash_table_put(&context->dynamic_table.reverse_lookup, &context->dynamic_table.buffer[i], (void *)i, NULL);
        aws_hash_table_put(
            &context->dynamic_table.reverse_lookup_name_only, &context->dynamic_table.buffer[i].name, (void *)i, NULL);
    }

    return AWS_OP_SUCCESS;
}

enum aws_hpack_decode_status aws_hpack_decode_integer(
    struct aws_hpack_context *context,
    struct aws_byte_cursor *to_decode,
    uint8_t prefix_size,
    uint64_t *integer) {

    AWS_PRECONDITION(context);
    AWS_PRECONDITION(to_decode);
    AWS_PRECONDITION(prefix_size <= 8);
    AWS_PRECONDITION(integer);

    /* #TODO once frame decoders go away, but for now this is necessary to avoid asserting in fuzz tests */
    if (to_decode->len == 0) {
        aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        return AWS_OP_ERR;
    }

    const uint8_t cut_bits = 8 - prefix_size;
    const uint8_t prefix_mask = UINT8_MAX >> cut_bits;

    struct hpack_progress_integer *progress = &context->progress_integer;
    enum aws_hpack_decode_status status = AWS_HPACK_DECODE_ONGOING;

    while (to_decode->len) {
        switch (progress->state) {
            case HPACK_INTEGER_STATE_INIT: {
                /* Read the first byte, and check whether this is it, or we need to continue */
                uint8_t byte = 0;
                bool succ = aws_byte_cursor_read_u8(to_decode, &byte);
                AWS_FATAL_ASSERT(succ);

                /* Cut the prefix */
                byte &= prefix_mask;

                /* No matter what, the first byte's value is always added to the integer */
                *integer = byte;

                if (byte != prefix_mask) {
                    status = AWS_HPACK_DECODE_COMPLETE;
                    goto handle_complete;
                }

                progress->state = HPACK_INTEGER_STATE_VALUE;

                /* Restart loop to make sure there's data available */
                continue;
            }

            case HPACK_INTEGER_STATE_VALUE: {
                uint8_t byte = 0;
                bool succ = aws_byte_cursor_read_u8(to_decode, &byte);
                AWS_FATAL_ASSERT(succ);

                uint64_t new_byte_value = (uint64_t)(byte & 127) << progress->bit_count;
                if (*integer + new_byte_value < *integer) {
                    goto overflow_detected;
                }
                *integer += new_byte_value;

                /* Check if we're done */
                if ((byte & 128) == 0) {
                    status = AWS_HPACK_DECODE_COMPLETE;
                    goto handle_complete;
                }

                /* Increment the bit count */
                progress->bit_count += 7;

                /* 7 Bits are expected to be used, so if we get to the point where any of
                 * those bits can't be used it's a decoding error */
                if (progress->bit_count > 64 - 7) {
                    goto overflow_detected;
                }
            }
        }
    }

    /* Fell out of data loop, must need more data */
    return AWS_HPACK_DECODE_ONGOING;

overflow_detected:
    aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
    status = AWS_HPACK_DECODE_ERROR;

handle_complete:
    /* Goto if done, or error */
    AWS_ZERO_STRUCT(context->progress_integer);
    return status;
}

size_t aws_hpack_get_encoded_length_string(
    struct aws_hpack_context *context,
    struct aws_byte_cursor to_encode,
    bool huffman_encode) {

    AWS_PRECONDITION(context);
    AWS_PRECONDITION(to_encode.ptr && to_encode.len);

    size_t length = 0;

    /* Get the header length */
    size_t encoded_length;
    if (huffman_encode) {
        encoded_length = aws_huffman_get_encoded_length(&context->encoder, to_encode);
    } else {
        encoded_length = to_encode.len;
    }
    length += aws_hpack_get_encoded_length_integer(encoded_length, 7);

    /* Add the string length */
    length += encoded_length;

    return length;
}

int aws_hpack_encode_string(
    struct aws_hpack_context *context,
    struct aws_byte_cursor *to_encode,
    bool huffman_encode,
    struct aws_byte_buf *output) {

    AWS_PRECONDITION(context);
    AWS_PRECONDITION(to_encode);
    AWS_PRECONDITION(output);

    if (output->len == output->capacity) {
        return aws_raise_error(AWS_ERROR_SHORT_BUFFER);
    }

    struct aws_byte_cursor to_encode_backup = *to_encode;

    /* Write the use_huffman bit */
    output->buffer[output->len] = huffman_encode << 7;

    /* Write the header */
    size_t encoded_length;
    if (huffman_encode) {
        encoded_length = aws_huffman_get_encoded_length(&context->encoder, *to_encode);
    } else {
        encoded_length = to_encode->len;
    }
    if (aws_hpack_encode_integer(encoded_length, 7, output)) {
        goto error;
    }

    if (huffman_encode) {
        aws_huffman_encoder_reset(&context->encoder);
        int result = aws_huffman_encode(&context->encoder, to_encode, output);
        if (result) {
            goto error;
        }
    } else {
        bool result = aws_byte_buf_write_from_whole_cursor(output, *to_encode);
        if (!result) {
            aws_raise_error(AWS_ERROR_SHORT_BUFFER);
            goto error;
        }
        aws_byte_cursor_advance(to_encode, to_encode->len);
    }
    return AWS_OP_SUCCESS;

error:
    *to_encode = to_encode_backup;
    return AWS_OP_ERR;
}

enum aws_hpack_decode_status aws_hpack_decode_string(
    struct aws_hpack_context *context,
    struct aws_byte_cursor *to_decode,
    struct aws_byte_buf *output) {

    AWS_PRECONDITION(context);
    AWS_PRECONDITION(to_decode);
    AWS_PRECONDITION(output);

    /* #TODO once frame decoders go away, but for now this is necessary to avoid asserting in fuzz tests */
    if (to_decode->len == 0) {
        aws_raise_error(AWS_ERROR_SHORT_BUFFER);
        return AWS_OP_ERR;
    }

    struct hpack_progress_string *progress = &context->progress_string;
    enum aws_hpack_decode_status status = AWS_HPACK_DECODE_ONGOING;

    switch (progress->state) {
        case HPACK_STRING_STATE_INIT: {
            /* Do init stuff */
            progress->state = HPACK_STRING_STATE_LENGTH;
            progress->use_huffman = *to_decode->ptr >> 7;
            aws_huffman_decoder_reset(&context->decoder);
            /* fallthrough, since we didn't consume any data */
        }
        /* FALLTHRU */
        case HPACK_STRING_STATE_LENGTH: {
            status = aws_hpack_decode_integer(context, to_decode, 7, &progress->length);
            if (status) {
                goto handle_complete;
            }
            if (progress->length > SIZE_MAX) {
                aws_raise_error(AWS_ERROR_OVERFLOW_DETECTED);
                status = AWS_HPACK_DECODE_ERROR;
                goto handle_complete;
            }

            /* If this was all of the data we had, return ONGOING */
            if (!to_decode->len) {
                return AWS_HPACK_DECODE_ONGOING;
            }
        }
        /* FALLTHRU */
        case HPACK_STRING_STATE_VALUE: {
            /* Take either as much data as we need, or as we can */
            size_t to_process = progress->length;
            bool will_finish = true;
            if (to_process > to_decode->len) {
                to_process = to_decode->len;
                will_finish = false;
            }

            struct aws_byte_cursor value = aws_byte_cursor_advance(to_decode, to_process);

            if (progress->use_huffman) {
                if (aws_huffman_decode(&context->decoder, &value, output)) {
                    status = AWS_HPACK_DECODE_ERROR;
                    goto handle_complete;
                }
            } else {
                if (!aws_byte_buf_write_from_whole_cursor(output, value)) {
                    aws_raise_error(AWS_ERROR_SHORT_BUFFER);
                    status = AWS_HPACK_DECODE_ERROR;
                    goto handle_complete;
                }
            }

            /* If whole length consumed, return complete */
            if (will_finish) {
                status = AWS_HPACK_DECODE_COMPLETE;
                goto handle_complete;
            }
        }
    }

    /* Fell out of to_decode loop, must still be in progress */
    return AWS_HPACK_DECODE_ONGOING;

handle_complete:
    /* Goto if done, or error */
    AWS_ZERO_STRUCT(context->progress_string);
    return status;
}
