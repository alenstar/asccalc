/*
 * Copyright (c) 2012 Alex Hornung <alex@alexhornung.com>.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <assert.h>

#include <gmp.h>
#include <mpfr.h>

#include "linenoise.h"

#include "hashtable.h"

#include "optype.h"
#include "num.h"
#include "var.h"
#include "ast.h"
#include "calc.h"
#include "func.h"
#include "safe_mem.h"
#include "lex.yy.h"

extern int nesting;
extern int linecont;


void
yyerror(const char *s, ...)
{
	va_list ap;

	va_start(ap, s);

	fprintf(stderr, "%d: error: ", yylineno);
	vfprintf(stderr, s, ap);
	fprintf(stderr, "\n");

	nesting = 0;
}

static
char *
prompt(void)
{
	static char buf[1024];
	int i;

	if (nesting || linecont) {
		snprintf(buf, sizeof(buf), "..");
		for (i = 0; i < nesting; i++)
			strcat(buf, "..");

		strcat(buf, " ");
	} else {
		snprintf(buf, sizeof(buf), "-> ");
	}

	return buf;
}


struct comp_helper {
	void *priv;
	const char *filter;
	size_t filter_len;
};


static
void
_var_completion_filter(void *priv, const char *var_name)
{
	struct comp_helper *ph = priv;

	if (strncmp(var_name, ph->filter, ph->filter_len) == 0) {
		linenoiseAddCompletion(ph->priv, var_name);
	}
}


static
void
linenoise_completion(const char *buf, linenoiseCompletions *lc) {
	struct comp_helper h;

	if (buf[0] == '\0') {
		linenoiseAddCompletion(lc, "ans ");
	} else {
		h.priv = lc;
		h.filter = buf;
		h.filter_len = strlen(buf);
		var_iterate(&h, _var_completion_filter);
	}
}


int
yywrap(void)
{
	char *line;
	int len;

	if (nesting || linecont) {
		if ((line = linenoise(prompt())) != NULL) {
			linenoiseHistoryAdd(line);
			len = strlen(line);
			line = realloc(line, len+2);
			line[len] = '\n';
			line[len+1] = '\0';
			yy_scan_string(line);
			free(line);
		}
		return 0;
	}

	return 1;
}


int
main(int argc, char *argv[])
{
	char *progname = argv[0];
	char *line;
	int len;

	varinit();
	num_init();
	funinit();

	linenoiseHistorySetMaxLen(MAX_HIST_LEN);
	linenoiseSetCompletionCallback(linenoise_completion);

	if (isatty(fileno(stdin))) {
		printf("ascalc %d.%d - A Simple Console Calculator\n",
		    MAJ_VER, MIN_VER);
		printf("Copyright (c) 2012-2014 Alex Hornung\n");
		printf("Type 'help' for available commands\n");
		printf("\n");
	}

	while ((line = linenoise(prompt())) != NULL) {
		linenoiseHistoryAdd(line);
		len = strlen(line);
		line = realloc(line, len + 2);
		line[len] = '\n';
		line[len+1] = '\0';
		yy_scan_string(line);
		free(line);
		yyparse();
	}

	return 0;
}


static char mode = 'd';
mpfr_rnd_t round_mode = MPFR_RNDN;
static int scientific_mode = 0;

void
mode_switch(char new_mode)
{
	mode = new_mode;
	round_mode = (new_mode == 'd' || new_mode == 's') ? MPFR_RNDN : MPFR_RNDZ;
	scientific_mode = (new_mode == 's');
}


void
test_print_num(num_t n)
{
	const char *prefix = "";
	char *s;
	num_t a;
	int base;

	switch (mode) {
	case 'b': base = 2; prefix = "0b"; break;
	case 'd': base = 10; prefix = ""; break;
	case 'x':
	case 'h': base = 16; prefix = "0x"; break;
	case 'o': base = 8; prefix = "0"; break;
	default: base = 10; prefix = "";
	}

	if (base != 10 && n->num_type != NUM_INT)
		a = num_new_z(N_TEMP, n);
	else
		a = n;

	if (a->num_type == NUM_INT) {
		if ((s = mpz_get_str(NULL, base, Z(a))) == NULL) {
			yyerror("ENOMEM");
			exit(1);
		}
		mpfr_printf("%s%s\n", prefix, s);
		free(s);
	} else if (a->num_type == NUM_FP) {
		if (scientific_mode) {
			mpfr_printf("%.R*G\n", round_mode, F(a));
		} else if (mpfr_integer_p(F(a))) {
			mpfr_printf("%.0R*f\n", round_mode, F(a));
		} else {
			mpfr_printf("%.R*f\n", round_mode, F(a));
		}
	} else {
		printf("invalid!\n");
	}
}


extern int nallocations;

void
go(ast_t a)
{
	var_t var;
	num_t ans;

	//printf("Allocations: %d\n", nallocations);
	ans = eval(a, NULL);

	if (ans == NULL)
		return;

	if (isatty(fileno(stdin)))
		printf("ans = ");
	test_print_num(ans);

	var = varlookup("ans", 1);
	if (var->v != NULL)
		num_delete(var->v);
	var->v = num_new_fp(0, ans);

	num_delete_temp();
	ast_delete(a);

	//printf("Allocations: %d\n", nallocations);
}


void
help(void)
{
	printf("Commands:\n");

	printf("\tls\t\t- Lists all variables\n\n");

	printf("\tlsfn\t\t- Lists all functions\n\n");

	printf("\tm <MODE>\t- Same as 'mode'\n\n");

	printf("\tmode <MODE>\t- Switches to <MODE>, where mode is one\n");
	printf("\t\t\t  of the following: b,d,s,h,o,x - for binary, decimal, \n");
	printf("\t\t\t  scientific decimal, hexadecimal, octal, hexadecimal, \n");
	printf("\t\t\t  output\n\n");
	printf("\tquit\t\t- Exits the program\n\n");
	printf("\texit\t\t- Exits the program\n\n");
}
