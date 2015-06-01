/* Copyright (c) 2014-2015 Dovecot authors, see the included COPYING file */

#include "lib.h"
#include "buffer.h"
#include "unichar.h"
#include "bsearch-insert-pos.h"
#include "fts-tokenizer-private.h"
#include "fts-tokenizer-generic-private.h"
#include "word-boundary-data.c"
#include "word-break-data.c"

#define FTS_DEFAULT_TOKEN_MAX_LENGTH 30

#define IS_NONASCII_APOSTROPHE(c) \
	((c) == 0x2019 || (c) == 0xFF07)
#define IS_APOSTROPHE(c) \
	((c) == 0x0027 || IS_NONASCII_APOSTROPHE(c))

static unsigned char fts_ascii_word_breaks[128] = {
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 0-15 */
	1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, /* 16-31 */

	1, 1, 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, /* 32-47:  !"#$%&()*+,-./ */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, /* 48-63: :;<=>? */
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 64-79: @ */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0, /* 80-95: [\]^ */
	1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, /* 96-111: ` */
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 0  /* 112-127: {|}~ */
};

static int
fts_tokenizer_generic_create(const char *const *settings,
			     struct fts_tokenizer **tokenizer_r,
			     const char **error_r)
{
	struct generic_fts_tokenizer *tok;
	unsigned int max_length = FTS_DEFAULT_TOKEN_MAX_LENGTH;
	enum boundary_algorithm algo = BOUNDARY_ALGORITHM_SIMPLE;
	unsigned int i;

	for (i = 0; settings[i] != NULL; i += 2) {
		const char *key = settings[i], *value = settings[i+1];

		if (strcmp(key, "maxlen") == 0) {
			if (str_to_uint(value, &max_length) < 0 ||
			    max_length == 0) {
				*error_r = t_strdup_printf(
					"Invalid maxlen setting: %s", value);
				return -1;
			}
		} else if (strcmp(key, "algorithm") == 0) {
			if (strcmp(value, ALGORITHM_TR29_NAME) == 0)
				algo = BOUNDARY_ALGORITHM_TR29;
			else if (strcmp(value, ALGORITHM_SIMPLE_NAME) == 0)
				;
			else {
				*error_r = t_strdup_printf(
				        "Invalid algorithm: %s", value);
				return -1;
			}
		} else if (strcmp(key, "search") == 0) {
			/* tokenizing a search string -
			   makes no difference to us */
		} else {
			*error_r = t_strdup_printf("Unknown setting: %s", key);
			return -1;
		}
	}

	tok = i_new(struct generic_fts_tokenizer, 1);
	if (algo == BOUNDARY_ALGORITHM_TR29)
		tok->tokenizer.v = &generic_tokenizer_vfuncs_tr29;
	else
		tok->tokenizer.v = &generic_tokenizer_vfuncs_simple;
	tok->max_length = max_length;
	tok->algorithm = algo;
	tok->token = buffer_create_dynamic(default_pool, 64);

	*tokenizer_r = &tok->tokenizer;
	return 0;
}

static void
fts_tokenizer_generic_destroy(struct fts_tokenizer *_tok)
{
	struct generic_fts_tokenizer *tok =
		(struct generic_fts_tokenizer *)_tok;

	buffer_free(&tok->token);
	i_free(tok);
}

static const char *fts_uni_strndup(const unsigned char *data, size_t size)
{
	size_t pos;

	/* if input is truncated with a partial UTF-8 character, drop it */
	(void)uni_utf8_partial_strlen_n(data, size, &pos);
	i_assert(pos > 0);
	return t_strndup(data, pos);
}

static bool
fts_tokenizer_generic_simple_current_token(struct generic_fts_tokenizer *tok,
                                           const char **token_r)
{
	const unsigned char *data;
	size_t start = 0, len;

	/* clean trailing and starting apostrophes. they were all made
	   into U+0027 earlier. */
	data = tok->token->data;
	len = tok->token->used;
	while (len > 0 && data[len - 1] == '\'')
		len--;
	while (start < len && data[start] == '\'')
		start++;

	*token_r = len - start == 0 ? "" :
		fts_uni_strndup(CONST_PTR_OFFSET(tok->token->data, start),
				len - start);
	buffer_set_used_size(tok->token, 0);
	return (*token_r)[0] != '\0';
}

static bool uint32_find(const uint32_t *data, unsigned int count,
			uint32_t value, unsigned int *idx_r)
{
	BINARY_NUMBER_SEARCH(data, count, value, idx_r);
}

static bool fts_uni_word_break(unichar_t c)
{
	unsigned int idx;

	/* Unicode General Punctuation, including deprecated characters. */
	if (c >= 0x2000 && c <= 0x206f)
		return TRUE;
	/* From word-break-data.c, which is generated from PropList.txt. */
	if (uint32_find(White_Space, N_ELEMENTS(White_Space), c, &idx))
		return TRUE;
	if (uint32_find(Dash, N_ELEMENTS(Dash), c, &idx))
		return TRUE;
	if (uint32_find(Quotation_Mark, N_ELEMENTS(Quotation_Mark), c, &idx))
		return TRUE;
	if (uint32_find(Terminal_Punctuation, N_ELEMENTS(Terminal_Punctuation), c, &idx))
		return TRUE;
	if (uint32_find(STerm, N_ELEMENTS(STerm), c, &idx))
		return TRUE;
	if (uint32_find(Pattern_White_Space, N_ELEMENTS(Pattern_White_Space), c, &idx))
		return TRUE;
	return FALSE;
}

static inline bool
fts_simple_is_word_break(struct generic_fts_tokenizer *tok,
			 unichar_t c, bool apostrophe)
{
	if (apostrophe)
		return tok->prev_letter == LETTER_TYPE_SINGLE_QUOTE;
	else if (c < 0x80)
		return fts_ascii_word_breaks[c] != 0;
	else
		return fts_uni_word_break(c);
}

static void fts_tokenizer_generic_reset(struct fts_tokenizer *_tok)
{
	struct generic_fts_tokenizer *tok =
		(struct generic_fts_tokenizer *)_tok;

	tok->prev_letter = LETTER_TYPE_NONE;
	tok->prev_prev_letter = LETTER_TYPE_NONE;
	buffer_set_used_size(tok->token, 0);
}

static void tok_append_truncated(struct generic_fts_tokenizer *tok,
				 const unsigned char *data, size_t size)
{
	size_t append_len, pos = 0, appended = 0;
	unichar_t c;

	i_assert(tok->max_length >= tok->token->used);
	append_len = I_MIN(size, tok->max_length - tok->token->used);

	/* Append only one kind of apostrophes. Simplifies things when returning
	   token. */
	while (pos < append_len) {
		if (uni_utf8_get_char_n(data + pos, size - pos, &c) <= 0)
			i_unreached();
		if (IS_NONASCII_APOSTROPHE(c)) {
			buffer_append(tok->token, data, pos);
			buffer_append_c(tok->token, '\'');
			appended = pos + 1;
		}
		pos += uni_utf8_char_bytes(data[pos]);
	}
	if (appended < append_len)
		buffer_append(tok->token, data + appended, append_len - appended);
}

static int
fts_tokenizer_generic_next_simple(struct fts_tokenizer *_tok,
                                  const unsigned char *data, size_t size,
				  size_t *skip_r, const char **token_r,
				  const char **error_r ATTR_UNUSED)
{
	struct generic_fts_tokenizer *tok =
		(struct generic_fts_tokenizer *)_tok;
	size_t i, start = 0;
	unsigned int char_size;
	unichar_t c;
	bool apostrophe;

	for (i = 0; i < size; i += char_size) {
		if (uni_utf8_get_char_n(data + i, size - i, &c) <= 0)
			i_unreached();
		char_size = uni_utf8_char_bytes(data[i]);

		apostrophe = IS_APOSTROPHE(c);
		if (fts_simple_is_word_break(tok, c, apostrophe)) {
			tok_append_truncated(tok, data + start, i - start);
			if (tok->token->used > 0 &&
			    fts_tokenizer_generic_simple_current_token(tok, token_r)) {
				*skip_r = i + char_size;
				return 1;
			}
			start = i + char_size;
			/* it doesn't actually matter at this point how whether
			   subsequent apostrophes are handled by prefix
			   skipping or by ignoring empty tokens - they will be
			   dropped in any case. */
			tok->prev_letter = LETTER_TYPE_NONE;
		} else {
			tok->prev_letter = apostrophe ?
				LETTER_TYPE_SINGLE_QUOTE : LETTER_TYPE_NONE;
		}
	}
	/* word boundary not found yet */
	tok_append_truncated(tok, data + start, i - start);
	*skip_r = i;

	/* return the last token */
	if (size == 0 && tok->token->used > 0) {
		if (fts_tokenizer_generic_simple_current_token(tok, token_r))
			return 1;
	}

	return 0;
}

/* TODO: Arrange array searches roughly in order of likelyhood of a match.
   TODO: Make some array of the arrays, so this can be a foreach loop.
   TODO: Check for Hangul.
   TODO: Add Hyphens U+002D HYPHEN-MINUS, U+2010 HYPHEN, possibly also
   U+058A ( ֊ ) ARMENIAN HYPHEN, and U+30A0 KATAKANA-HIRAGANA DOUBLE
   HYPHEN.
   TODO
*/
static enum letter_type letter_type(unichar_t c)
{
	unsigned int idx;

	if (IS_APOSTROPHE(c))
		return LETTER_TYPE_APOSTROPHE;
	if (uint32_find(CR, N_ELEMENTS(CR), c, &idx))
		return LETTER_TYPE_CR;
	if (uint32_find(LF, N_ELEMENTS(LF), c, &idx))
		return LETTER_TYPE_LF;
	if (uint32_find(Newline, N_ELEMENTS(Newline), c, &idx))
		return LETTER_TYPE_NEWLINE;
	if (uint32_find(Extend, N_ELEMENTS(Extend), c, &idx))
		return LETTER_TYPE_EXTEND;
	if (uint32_find(Regional_Indicator, N_ELEMENTS(Regional_Indicator), c, &idx))
		return LETTER_TYPE_REGIONAL_INDICATOR;
	if (uint32_find(Format, N_ELEMENTS(Format), c, &idx))
		return LETTER_TYPE_FORMAT;
	if (uint32_find(Katakana, N_ELEMENTS(Katakana), c, &idx))
		return LETTER_TYPE_KATAKANA;
	if (uint32_find(Hebrew_Letter, N_ELEMENTS(Hebrew_Letter), c, &idx))
		return LETTER_TYPE_HEBREW_LETTER;
	if (uint32_find(ALetter, N_ELEMENTS(ALetter), c, &idx))
		return LETTER_TYPE_ALETTER;
	if (uint32_find(Single_Quote, N_ELEMENTS(Single_Quote), c, &idx))
		return LETTER_TYPE_SINGLE_QUOTE;
	if (uint32_find(Double_Quote, N_ELEMENTS(Double_Quote), c, &idx))
		return LETTER_TYPE_DOUBLE_QUOTE;
	if (uint32_find(MidNumLet, N_ELEMENTS(MidNumLet), c, &idx))
		return LETTER_TYPE_MIDNUMLET;
	if (uint32_find(MidLetter, N_ELEMENTS(MidLetter), c, &idx))
		return LETTER_TYPE_MIDLETTER;
	if (uint32_find(MidNum, N_ELEMENTS(MidNum), c, &idx))
		return LETTER_TYPE_MIDNUM;
	if (uint32_find(Numeric, N_ELEMENTS(Numeric), c, &idx))
		return LETTER_TYPE_NUMERIC;
	if (uint32_find(ExtendNumLet, N_ELEMENTS(ExtendNumLet), c, &idx))
		return LETTER_TYPE_EXTENDNUMLET;
	return LETTER_TYPE_OTHER;
}

static bool letter_panic(struct generic_fts_tokenizer *tok ATTR_UNUSED)
{
	i_panic("Letter type should not be used.");
}

/* WB3, WB3a and WB3b, but really different since we try to eat
   whitespace between words. */
static bool letter_cr_lf_newline(struct generic_fts_tokenizer *tok ATTR_UNUSED)
{
	return TRUE;
}

static bool letter_extend_format(struct generic_fts_tokenizer *tok ATTR_UNUSED)
{
	/* WB4 */
	return FALSE;
}

static bool letter_regional_indicator(struct generic_fts_tokenizer *tok)
{
	/* WB13c */
	if (tok->prev_letter == LETTER_TYPE_REGIONAL_INDICATOR)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_katakana(struct generic_fts_tokenizer *tok)
{
	/* WB13 */
	if (tok->prev_letter == LETTER_TYPE_KATAKANA)
		return FALSE;

	/* WB13b */
	if (tok->prev_letter == LETTER_TYPE_EXTENDNUMLET)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_hebrew(struct generic_fts_tokenizer *tok)
{
	/* WB5 */
	if (tok->prev_letter == LETTER_TYPE_HEBREW_LETTER)
		return FALSE;

	/* WB7 WB7c, except MidNumLet */
	if (tok->prev_prev_letter == LETTER_TYPE_HEBREW_LETTER &&
	    (tok->prev_letter == LETTER_TYPE_SINGLE_QUOTE ||
	     tok->prev_letter == LETTER_TYPE_APOSTROPHE ||
	     tok->prev_letter == LETTER_TYPE_MIDLETTER ||
	     tok->prev_letter == LETTER_TYPE_DOUBLE_QUOTE))
		return FALSE;

	/* WB10 */
	if (tok->prev_letter == LETTER_TYPE_NUMERIC)
		return FALSE;

	/* WB13b */
	if (tok->prev_letter == LETTER_TYPE_EXTENDNUMLET)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_aletter(struct generic_fts_tokenizer *tok)
{
	/* WB5 */
	if (tok->prev_letter == LETTER_TYPE_ALETTER)
		return FALSE;

	/* WB7, except MidNumLet */
	if (tok->prev_prev_letter == LETTER_TYPE_ALETTER &&
	    (tok->prev_letter == LETTER_TYPE_SINGLE_QUOTE ||
	     tok->prev_letter == LETTER_TYPE_APOSTROPHE ||
	     tok->prev_letter == LETTER_TYPE_MIDLETTER))
		return FALSE;

	/* WB10 */
	if (tok->prev_letter == LETTER_TYPE_NUMERIC)
		return FALSE;

	/* WB13b */
	if (tok->prev_letter == LETTER_TYPE_EXTENDNUMLET)
		return FALSE;


	return TRUE; /* Any / Any */
}

static bool letter_single_quote(struct generic_fts_tokenizer *tok)
{
	/* WB6 */
	if (tok->prev_letter == LETTER_TYPE_ALETTER ||
	    tok->prev_letter == LETTER_TYPE_HEBREW_LETTER)
		return FALSE;

	/* WB12 */
	if (tok->prev_letter == LETTER_TYPE_NUMERIC)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_double_quote(struct generic_fts_tokenizer *tok)
{

	if (tok->prev_letter == LETTER_TYPE_DOUBLE_QUOTE)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_midnumlet(struct generic_fts_tokenizer *tok ATTR_UNUSED)
{

	/* Break at MidNumLet, non-conformant with WB6/WB7 */
	return TRUE;
}

static bool letter_midletter(struct generic_fts_tokenizer *tok)
{
	/* WB6 */
	if (tok->prev_letter == LETTER_TYPE_ALETTER ||
	    tok->prev_letter == LETTER_TYPE_HEBREW_LETTER)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_midnum(struct generic_fts_tokenizer *tok)
{
	/* WB12 */
	if (tok->prev_letter == LETTER_TYPE_NUMERIC)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_numeric(struct generic_fts_tokenizer *tok)
{
	/* WB8 */
	if (tok->prev_letter == LETTER_TYPE_NUMERIC)
		return FALSE;

	/* WB9 */
	if (tok->prev_letter == LETTER_TYPE_ALETTER ||
	    tok->prev_letter == LETTER_TYPE_HEBREW_LETTER)
		return FALSE;

	/* WB11 */
	if(tok->prev_prev_letter == LETTER_TYPE_NUMERIC &&
	   (tok->prev_letter == LETTER_TYPE_MIDNUM ||
	    tok->prev_letter == LETTER_TYPE_MIDNUMLET ||
	    tok->prev_letter == LETTER_TYPE_SINGLE_QUOTE))
		return FALSE;

	/* WB13b */
	if (tok->prev_letter == LETTER_TYPE_EXTENDNUMLET)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_extendnumlet(struct generic_fts_tokenizer *tok)
{

	/* WB13a */
	if (tok->prev_letter == LETTER_TYPE_ALETTER ||
	    tok->prev_letter == LETTER_TYPE_HEBREW_LETTER ||
	    tok->prev_letter == LETTER_TYPE_NUMERIC ||
	    tok->prev_letter == LETTER_TYPE_KATAKANA ||
	    tok->prev_letter == LETTER_TYPE_EXTENDNUMLET)
		return FALSE;

	return TRUE; /* Any / Any */
}

static bool letter_apostrophe(struct generic_fts_tokenizer *tok)
{

       if (tok->prev_letter == LETTER_TYPE_ALETTER ||
           tok->prev_letter == LETTER_TYPE_HEBREW_LETTER)
               return FALSE;

       return TRUE; /* Any / Any */
}
static bool letter_other(struct generic_fts_tokenizer *tok ATTR_UNUSED)

{
	return TRUE; /* Any / Any */
}

static void
add_prev_letter(struct generic_fts_tokenizer *tok, enum letter_type lt)
{
	if(tok->prev_letter != LETTER_TYPE_NONE) {
		tok->prev_prev_letter = tok->prev_letter;
		tok->prev_letter = lt;
	} else
		tok->prev_letter = lt;
}

/*
   TODO: Define what to skip between words.
   TODO: Include double quotation marks? Messes up parsing?
   TODO: Does this "reverse approach" include too much in "whitespace"?
   TODO: Possibly use is_word_break()?
 */
static bool is_nontoken(enum letter_type lt)
{
	if (lt == LETTER_TYPE_REGIONAL_INDICATOR || lt == LETTER_TYPE_KATAKANA ||
	    lt == LETTER_TYPE_HEBREW_LETTER || lt == LETTER_TYPE_ALETTER ||
	    lt == LETTER_TYPE_NUMERIC)
		return FALSE;

	return TRUE;
}

/* The way things are done WB6/7 and WB11/12 "false positives" can
   leave trailing unwanted chars. They are searched for here. This is
   very kludgy and should be coded into the rules themselves
   somehow.
*/
static bool is_one_past_end(struct generic_fts_tokenizer *tok)
{
	/* WB6/7 false positive detected at one past end. */
	if (tok->prev_letter == LETTER_TYPE_MIDLETTER ||
	    tok->prev_letter == LETTER_TYPE_MIDNUMLET ||
	    tok->prev_letter == LETTER_TYPE_APOSTROPHE ||
	    tok->prev_letter == LETTER_TYPE_SINGLE_QUOTE )
		return TRUE;

	/* WB11/12 false positive detected at one past end. */
	if (tok->prev_letter == LETTER_TYPE_MIDNUM ||
	    tok->prev_letter == LETTER_TYPE_MIDNUMLET ||
	    tok->prev_letter == LETTER_TYPE_APOSTROPHE ||
	    tok->prev_letter == LETTER_TYPE_SINGLE_QUOTE)
		return TRUE;

	return FALSE;
}
static void
fts_tokenizer_generic_tr29_current_token(struct generic_fts_tokenizer *tok,
                                         const char **token_r)
{
	const unsigned char *data = tok->token->data;
	ssize_t len = tok->token->used;

	if (is_one_past_end(tok)) {
		/* delete the last character */
		while ((data[len-1] & 0x80) != 0)
			len--;
		i_assert(len > 0);
		len--;
	}
	/* we're skipping all non-token chars at the beginning of the word,
	   so by this point we must have something here - even if we just
	   deleted the last character */
	i_assert(len > 0);

	tok->prev_prev_letter = LETTER_TYPE_NONE;
	tok->prev_letter = LETTER_TYPE_NONE;

	*token_r = fts_uni_strndup(data, len);
	buffer_set_used_size(tok->token, 0);
}

struct letter_fn {
	bool (*fn)(struct generic_fts_tokenizer *tok);
};
static struct letter_fn letter_fns[] = {
	{letter_panic}, {letter_cr_lf_newline}, {letter_cr_lf_newline},
	{letter_cr_lf_newline}, {letter_extend_format},
	{letter_regional_indicator}, {letter_extend_format},
	{letter_katakana}, {letter_hebrew}, {letter_aletter},
	{letter_single_quote}, {letter_double_quote},
	{letter_midnumlet}, {letter_midletter}, {letter_midnum},
	{letter_numeric}, {letter_extendnumlet}, {letter_panic},
	{letter_panic}, {letter_apostrophe}, {letter_other}
};

/*
  Find word boundaries in input text. Based on Unicode standard annex
  #29, but tailored for FTS purposes.
  http://www.unicode.org/reports/tr29/

  Adaptions:
  * No word boundary at Start-Of-Text or End-of-Text (Wb1 and WB2).
  * Break just once, not before and after.
  * Break at MidNumLet, except apostrophes (diverging from WB6/WB7).
  * Other things also (e.g. is_nontoken(), not really pure tr29. Meant
  to assist in finding individual words.
*/
static bool
uni_found_word_boundary(struct generic_fts_tokenizer *tok, enum letter_type lt)
{
	/* No rule knows what to do with just one char, except the linebreaks
	   we eat away (above) anyway. */
	if (tok->prev_letter != LETTER_TYPE_NONE) {
		if (letter_fns[lt].fn(tok))
			return TRUE;
	}

	if (lt == LETTER_TYPE_EXTEND || lt == LETTER_TYPE_FORMAT) {
		/* These types are completely ignored. */
	} else {
		add_prev_letter(tok,lt);
	}
	return FALSE;
}

static int
fts_tokenizer_generic_next_tr29(struct fts_tokenizer *_tok,
				const unsigned char *data, size_t size,
				size_t *skip_r, const char **token_r,
				const char **error_r ATTR_UNUSED)
{
	struct generic_fts_tokenizer *tok =
		(struct generic_fts_tokenizer *)_tok;
	unichar_t c;
	size_t i, char_start_i, start_skip = 0;
	enum letter_type lt;

	/* TODO: Process 8bit chars separately, to speed things up. */
	for (i = 0; i < size; ) {
		char_start_i = i;
		if (uni_utf8_get_char_n(data + i, size - i, &c) <= 0)
			i_unreached();
		i += uni_utf8_char_bytes(data[i]);
		lt = letter_type(c);
		if (tok->prev_letter == LETTER_TYPE_NONE && is_nontoken(lt)) {
			/* Skip non-token chars at the beginning of token */
			i_assert(tok->token->used == 0);
			start_skip = i;
			continue;
		}
		if (uni_found_word_boundary(tok, lt)) {
			i_assert(char_start_i >= start_skip && size >= start_skip);
			tok_append_truncated(tok, data + start_skip,
					     char_start_i - start_skip);
			*skip_r = i;
			fts_tokenizer_generic_tr29_current_token(tok, token_r);
			return 1;
		}
	}
	i_assert(i >= start_skip && size >= start_skip);
	tok_append_truncated(tok, data + start_skip, i - start_skip);
	*skip_r = i;

	if (size == 0 && tok->token->used > 0) {
		/* return the last token */
		*skip_r = 0;
		fts_tokenizer_generic_tr29_current_token(tok, token_r);
		return 1;
	}
	return 0;
}

static int
fts_tokenizer_generic_next(struct fts_tokenizer *_tok ATTR_UNUSED,
			   const unsigned char *data ATTR_UNUSED,
                           size_t size ATTR_UNUSED,
                           size_t *skip_r ATTR_UNUSED,
			   const char **token_r ATTR_UNUSED,
			   const char **error_r ATTR_UNUSED)
{
	i_unreached();
}

static const struct fts_tokenizer_vfuncs generic_tokenizer_vfuncs = {
	fts_tokenizer_generic_create,
	fts_tokenizer_generic_destroy,
	fts_tokenizer_generic_reset,
	fts_tokenizer_generic_next
};

static const struct fts_tokenizer fts_tokenizer_generic_real = {
	.name = "generic",
	.v = &generic_tokenizer_vfuncs
};
const struct fts_tokenizer *fts_tokenizer_generic = &fts_tokenizer_generic_real;

const struct fts_tokenizer_vfuncs generic_tokenizer_vfuncs_simple = {
	fts_tokenizer_generic_create,
	fts_tokenizer_generic_destroy,
	fts_tokenizer_generic_reset,
	fts_tokenizer_generic_next_simple
};
const struct fts_tokenizer_vfuncs generic_tokenizer_vfuncs_tr29 = {
	fts_tokenizer_generic_create,
	fts_tokenizer_generic_destroy,
	fts_tokenizer_generic_reset,
	fts_tokenizer_generic_next_tr29
};
