/*
 * This file is part of cparser.
 * Copyright (C) 2007-2008 Matthias Braun <matze@braunis.de>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include <config.h>

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#include <assert.h>

#ifdef _WIN32

#include <fcntl.h>
#include <io.h>

/* no eXecute on Win32 */
#define X_OK 0
#define W_OK 2
#define R_OK 4

#define O_RDWR          _O_RDWR
#define O_CREAT         _O_CREAT
#define O_EXCL          _O_EXCL
#define O_BINARY        _O_BINARY

/* remap some names, we are not in the POSIX world */
#define access(fname, mode)      _access(fname, mode)
#define mktemp(tmpl)             _mktemp(tmpl)
#define open(fname, oflag, mode) _open(fname, oflag, mode)
#define fdopen(fd, mode)         _fdopen(fd, mode)
#define popen(cmd, mode)         _popen(cmd, mode)
#define pclose(file)             _pclose(file)

#else
#include <unistd.h>
#define HAVE_MKSTEMP
#endif

#ifndef WITH_LIBCORE
#define WITH_LIBCORE
#endif

#include <libfirm/firm.h>
#include <libfirm/be.h>

#include "lexer.h"
#include "token_t.h"
#include "types.h"
#include "type_hash.h"
#include "parser.h"
#include "ast2firm.h"
#include "diagnostic.h"
#include "lang_features.h"
#include "driver/firm_opt.h"
#include "driver/firm_cmdline.h"
#include "adt/error.h"
#include "write_fluffy.h"
#include "write_caml.h"
#include "revision.h"
#include "warning.h"

#ifndef PREPROCESSOR
#define PREPROCESSOR "cpp -std=c99 -U__WCHAR_TYPE__ -D__WCHAR_TYPE__=int -D__SIZE_TYPE__=__SIZE_TYPE__ -m32"
#endif

#ifndef LINKER
#define LINKER    "gcc -m32"
#endif

#ifndef ASSEMBLER
#define ASSEMBLER "as --32"
#endif

/** The current c mode/dialect. */
unsigned int c_mode = _C89|_C99|_GNUC;

/** The 'machine size', 16, 32 or 64 bit, 32bit is the default. */
unsigned int machine_size = 32;

/** true if the char type is signed. */
bool char_is_signed = true;

/** true for strict language checking. */
bool strict_mode = false;

/** use builtins for some libc functions */
bool use_builtins = false;

/* to switch on printing of implicit casts */
extern bool print_implicit_casts;

/* to switch on printing of parenthesis to indicate operator precedence */
extern bool print_parenthesis;

static int            verbose;
static struct obstack cppflags_obst, ldflags_obst;

typedef struct file_list_entry_t file_list_entry_t;

struct file_list_entry_t {
	const char        *filename;
	file_list_entry_t *next;
};

#if defined(_DEBUG) || defined(FIRM_DEBUG)
/**
 * Debug printf implementation.
 *
 * @param fmt  printf style format parameter
 */
void dbg_printf(const char *fmt, ...)
{
	va_list list;

	if (firm_dump.debug_print) {
		va_start(list, fmt);
		vprintf(fmt, list);
		va_end(list);
	}  /* if */
}
#endif /* defined(_DEBUG) || defined(FIRM_DEBUG) */

static void initialize_firm(void)
{
	firm_early_init();

	dump_consts_local(1);
	dump_keepalive_edges(1);
}

static void get_output_name(char *buf, size_t buflen, const char *inputname,
                            const char *newext)
{
	size_t last_dot = 0xffffffff;
	size_t i = 0;

	if(inputname == NULL) {
		snprintf(buf, buflen, "a%s", newext);
		return;
	}

	for(const char *c = inputname; *c != 0; ++c) {
		if(*c == '.')
			last_dot = i;
		++i;
	}
	if(last_dot == 0xffffffff)
		last_dot = i;

	if(last_dot >= buflen)
		panic("filename too long");
	memcpy(buf, inputname, last_dot);

	size_t extlen = strlen(newext) + 1;
	if(extlen + last_dot >= buflen)
		panic("filename too long");
	memcpy(buf+last_dot, newext, extlen);
}

#include "builtins.h"

static translation_unit_t *do_parsing(FILE *const in, const char *const input_name)
{
	start_parsing();

	if (use_builtins) {
		lexer_open_buffer(builtins, sizeof(builtins)-1, "<builtin>");
		parse();
	}

	lexer_open_stream(in, input_name);
	parse();

	translation_unit_t *unit = finish_parsing();
	return unit;
}

static void lextest(FILE *in, const char *fname)
{
	lexer_open_stream(in, fname);

	do {
		lexer_next_preprocessing_token();
		print_token(stdout, &lexer_token);
		puts("");
	} while(lexer_token.type != T_EOF);
}

static FILE *preprocess(FILE *in, const char *fname, bool to_stdout)
{
	char buf[4096];
	obstack_1grow(&cppflags_obst, '\0');
	const char *flags = obstack_finish(&cppflags_obst);

	if(in != stdin) {
		snprintf(buf, sizeof(buf), PREPROCESSOR " %s %s", flags, fname);
	} else {
		/* read from stdin */
		snprintf(buf, sizeof(buf), PREPROCESSOR " %s -", flags);
	}

	if(verbose) {
		puts(buf);
	}
	if(to_stdout) {
		int res = system(buf);
		exit(res);
	} else {
		FILE *f = popen(buf, "r");
		if(f == NULL) {
			fprintf(stderr, "invoking preprocessor failed\n");
			exit(1);
		}
		return f;
	}
}

static void do_link(const char *out, const char *in)
{
	char buf[4096];
	obstack_1grow(&ldflags_obst, '\0');
	const char *flags = obstack_finish(&ldflags_obst);

	snprintf(buf, sizeof(buf), LINKER " %s -o %s %s", flags, out, in);
	if(verbose) {
		puts(buf);
	}
	int err = system(buf);
	if(err != 0) {
		fprintf(stderr, "linker reported an error\n");
		exit(1);
	}
}

static void assemble(const char *out, const char *in)
{
	char buf[4096];

	snprintf(buf, sizeof(buf), "%s %s -o %s", ASSEMBLER, in, out);
	if(verbose) {
		puts(buf);
	}

	int err = system(buf);
	if(err != 0) {
		fprintf(stderr, "assembler reported an error\n");
		exit(1);
	}
}

static const char *try_dir(const char *dir)
{
	if(dir == NULL)
		return dir;
	if(access(dir, R_OK | W_OK | X_OK) == 0)
		return dir;
	return NULL;
}

static const char *get_tempdir(void)
{
	static const char *tmpdir = NULL;

	if(tmpdir != NULL)
		return tmpdir;

	if(tmpdir == NULL)
		tmpdir = try_dir(getenv("TMPDIR"));
	if(tmpdir == NULL)
		tmpdir = try_dir(getenv("TMP"));
	if(tmpdir == NULL)
		tmpdir = try_dir(getenv("TEMP"));

#ifdef P_tmpdir
	if(tmpdir == NULL)
		tmpdir = try_dir(P_tmpdir);
#endif

	if(tmpdir == NULL)
		tmpdir = try_dir("/var/tmp");
	if(tmpdir == NULL)
		tmpdir = try_dir("/usr/tmp");
	if(tmpdir == NULL)
		tmpdir = try_dir("/tmp");

	if(tmpdir == NULL)
		tmpdir = ".";

	return tmpdir;
}

#ifndef HAVE_MKSTEMP
/* cheap and nasty mkstemp replacement */
static int mkstemp(char *templ)
{
	mktemp(templ);
	return open(templ, O_RDWR|O_CREAT|O_EXCL|O_BINARY, 0600);
}
#endif

/**
 * an own version of tmpnam, which: writes in a buffer, appends a user specified
 * suffix, emits no warnings during linking (like glibc/gnu ld do for tmpnam)...
 */
static FILE *make_temp_file(char *buffer, size_t buflen,
                            const char *prefix, const char *suffix)
{
	const char *tempdir = get_tempdir();

	/* oh well... mkstemp doesn't accept a suffix after XXXXXX... */
	(void) suffix;
	suffix = "";

	snprintf(buffer, buflen, "%s/%sXXXXXX%s", tempdir, prefix, suffix);

	int fd = mkstemp(buffer);
	if(fd == -1) {
		fprintf(stderr, "couldn't create temporary file: %s\n",
		        strerror(errno));
		exit(1);
	}
	FILE *out = fdopen(fd, "w");
	if(out == NULL) {
		fprintf(stderr, "couldn't create temporary file FILE*\n");
		exit(1);
	}

	return out;
}

/**
 * Do the necessary lowering for compound parameters.
 */
void lower_compound_params(void)
{
	lower_params_t params;

	params.def_ptr_alignment    = 4;
	params.flags                = LF_COMPOUND_RETURN | LF_RETURN_HIDDEN;
	params.hidden_params        = ADD_HIDDEN_ALWAYS_IN_FRONT;
	params.find_pointer_type    = NULL;
	params.ret_compound_in_regs = NULL;
	lower_calls_with_compounds(&params);
}

typedef enum compile_mode_t {
	BenchmarkParser,
	PreprocessOnly,
	ParseOnly,
	Compile,
	CompileDump,
	CompileAssemble,
	CompileAssembleLink,
	LexTest,
	PrintAst,
	PrintFluffy,
	PrintCaml,
	Link
} compile_mode_t;

static void usage(const char *argv0)
{
	fprintf(stderr, "Usage %s input [-o output] [-c]\n", argv0);
}

static void print_cparser_version(void) {
	firm_version_t ver;
	firm_get_version(&ver);

	printf("cparser (%s) using libFirm (%u.%u",
		cparser_REVISION, ver.major, ver.minor);
	if(ver.revision[0] != 0) {
		putchar(' ');
		fputs(ver.revision, stdout);
	}
	if(ver.build[0] != 0) {
		putchar(' ');
		fputs(ver.build, stdout);
	}
	puts(")\n");
}

static void set_be_option(const char *arg)
{
	int res = firm_be_option(arg);
	(void) res;
	assert(res);
}

static void set_option(const char *arg)
{
	int res = firm_option(arg);
	(void) res;
	assert(res);
}

int main(int argc, char **argv)
{
	initialize_firm();

	const char        *input        = NULL;
	const char        *outname      = NULL;
	const char        *dumpfunction = NULL;
	compile_mode_t     mode         = CompileAssembleLink;
	int                opt_level    = 1;
	int                result       = EXIT_SUCCESS;
	char               cpu_arch[16] = "ia32";
	file_list_entry_t *c_files      = NULL;
	file_list_entry_t *s_files      = NULL;
	file_list_entry_t *o_files      = NULL;
	struct obstack     file_obst;

	obstack_init(&cppflags_obst);
	obstack_init(&ldflags_obst);
	obstack_init(&file_obst);

#define GET_ARG_AFTER(def, args)                                             \
	def = &arg[sizeof(args)-1];                                              \
	if(def[0] == '\0') {                                                     \
		++i;                                                                 \
		if(i >= argc) {                                                      \
			fprintf(stderr, "error: expected argument after '" args "'\n");  \
			argument_errors = true;                                          \
			break;                                                           \
		}                                                                    \
		def = argv[i];                                                       \
		if(def[0] == '-' && def[1] != '\0') {                                \
			fprintf(stderr, "error: expected argument after '" args "'\n");  \
			argument_errors = true;                                          \
			continue;                                                        \
		}                                                                    \
	}

#define SINGLE_OPTION(ch) (option[0] == (ch) && option[1] == '\0')

	/* early options parsing (find out optimisation level) */
	for(int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if(arg[0] != '-')
			continue;

		const char *option = &arg[1];
		if(option[0] == 'O') {
			sscanf(&option[1], "%d", &opt_level);
		}
	}

	/* apply optimisation level */
	switch(opt_level) {
	case 0:
		set_option("no-opt");
		break;
	case 1:
		set_option("no-inline");
		break;
	default:
	case 4:
		set_option("strict-aliasing");
		/* fallthrough */
	case 3:
		set_option("cond-eval");
		set_option("if-conv");
		use_builtins = true;
		/* fallthrough */
	case 2:
		set_option("inline");
		set_option("deconv");
		set_be_option("omitfp");
		break;
	}

	/* parse rest of options */
	bool help_displayed  = false;
	bool argument_errors = false;
	for(int i = 1; i < argc; ++i) {
		const char *arg = argv[i];
		if(arg[0] == '-' && arg[1] != 0) {
			/* an option */
			const char *option = &arg[1];
			if(option[0] == 'o') {
				GET_ARG_AFTER(outname, "-o");
			} else if(option[0] == 'g') {
				set_be_option("debuginfo=stabs");
				set_be_option("omitfp=no");
				set_be_option("ia32-nooptcc=yes");
			} else if(SINGLE_OPTION('c')) {
				mode = CompileAssemble;
			} else if(SINGLE_OPTION('E')) {
				mode = PreprocessOnly;
			} else if(SINGLE_OPTION('S')) {
				mode = Compile;
			} else if(option[0] == 'O') {
				continue;
			} else if(option[0] == 'I') {
				const char *opt;
				GET_ARG_AFTER(opt, "-I");
				obstack_printf(&cppflags_obst, " -I%s", opt);
			} else if(option[0] == 'D') {
				const char *opt;
				GET_ARG_AFTER(opt, "-D");
				obstack_printf(&cppflags_obst, " -D%s", opt);
			} else if(option[0] == 'U') {
				const char *opt;
				GET_ARG_AFTER(opt, "-U");
				obstack_printf(&cppflags_obst, " -U%s", opt);
			} else if(option[0] == 'l') {
				const char *opt;
				GET_ARG_AFTER(opt, "-l");
				obstack_printf(&ldflags_obst, " -l%s", opt);
			} else if(option[0] == 'L') {
				const char *opt;
				GET_ARG_AFTER(opt, "-L");
				obstack_printf(&ldflags_obst, " -L%s", opt);
			} else if(SINGLE_OPTION('v')) {
				verbose = 1;
			} else if(SINGLE_OPTION('w')) {
				inhibit_all_warnings = true;
			} else if(option[0] == 'f') {
				const char *opt;
				GET_ARG_AFTER(opt, "-f");

				if(strcmp(opt, "syntax-only") == 0) {
					mode = ParseOnly;
				} else if(strcmp(opt, "omit-frame-pointer") == 0) {
					set_be_option("omitfp");
				} else if(strcmp(opt, "no-omit-frame-pointer") == 0) {
					set_be_option("omitfp=no");
				} else {
					int res = firm_option(opt);
					if (res == 0) {
						fprintf(stderr, "error: unknown Firm option '-f %s'\n",
						        opt);
						argument_errors = true;
						continue;
					} else if (res == -1) {
						help_displayed = true;
					}
				}
			} else if(option[0] == 'b') {
				const char *opt;
				GET_ARG_AFTER(opt, "-b");
				int res = firm_be_option(opt);
				if (res == 0) {
					fprintf(stderr, "error: unknown Firm backend option '-b %s'\n",
					        opt);
					argument_errors = true;
				} else if (res == -1) {
					help_displayed = true;
				} else {
					if (strncmp(opt, "isa=", 4) == 0)
						strncpy(cpu_arch, opt, sizeof(cpu_arch));
				}
			} else if(option[0] == 'W') {
				set_warning_opt(&option[1]);
			} else if(option[0] == 'm') {
				/* -m options */
				const char *opt;
				char arch_opt[64];

				GET_ARG_AFTER(opt, "-m");
				if(strncmp(opt, "arch=", 5) == 0) {
					GET_ARG_AFTER(opt, "-march=");
					snprintf(arch_opt, sizeof(arch_opt), "%s-arch=%s", cpu_arch, opt);
					int res = firm_be_option(arch_opt);
					if (res == 0)
						argument_errors = true;
					else {
						snprintf(arch_opt, sizeof(arch_opt), "%s-opt=%s", cpu_arch, opt);
						int res = firm_be_option(arch_opt);
						if (res == 0)
							argument_errors = true;
					}
				} else if(strncmp(opt, "tune=", 5) == 0) {
					GET_ARG_AFTER(opt, "-mtune=");
					snprintf(arch_opt, sizeof(arch_opt), "%s-opt=%s", cpu_arch, opt);
					int res = firm_be_option(arch_opt);
					if (res == 0)
						argument_errors = true;
				} else if(strncmp(opt, "cpu=", 4) == 0) {
					GET_ARG_AFTER(opt, "-mcpu=");
					snprintf(arch_opt, sizeof(arch_opt), "%s-arch=%s", cpu_arch, opt);
					int res = firm_be_option(arch_opt);
					if (res == 0)
						argument_errors = true;
				} else if(strncmp(opt, "fpmath=", 7) == 0) {
					GET_ARG_AFTER(opt, "-mfpmath=");
					if(strcmp(opt, "387") == 0)
						opt = "x87";
					else if(strcmp(opt, "sse") == 0)
						opt = "sse2";
					else {
						fprintf(stderr, "error: option -mfpumath supports only 387 or sse\n");
						argument_errors = true;
					}
					if(!argument_errors) {
						snprintf(arch_opt, sizeof(arch_opt), "%s-fpunit=%s", cpu_arch, opt);
						int res = firm_be_option(arch_opt);
						if (res == 0)
							argument_errors = true;
					}
				} else if(strncmp(opt, "preferred-stack-boundary=", 25) == 0) {
					GET_ARG_AFTER(opt, "-mpreferred-stack-boundary=");
					snprintf(arch_opt, sizeof(arch_opt), "%s-stackalign=%s", cpu_arch, opt);
					int res = firm_be_option(arch_opt);
					if (res == 0)
						argument_errors = true;
				} else if(strcmp(opt, "omit-leaf-frame-pointer") == 0) {
					set_be_option("omitleaffp=1");
				} else if(strcmp(opt, "no-omit-leaf-frame-pointer") == 0) {
					set_be_option("omitleaffp=0");
				} else {
					char *endptr;
					long int value = strtol(opt, &endptr, 10);
					if (*endptr != '\0') {
						fprintf(stderr, "error: wrong option '-m %s'\n",  opt);
						argument_errors = true;
					}
					if (value != 16 && value != 32 && value != 64) {
						fprintf(stderr, "error: option -m supports only 16, 32 or 64\n");
						argument_errors = true;
					} else {
						machine_size = (unsigned int)value;
					}
				}
			} else if (option[0] == '\0') {
				if(input != NULL) {
					fprintf(stderr, "error: multiple input files specified\n");
					argument_errors = true;
				} else {
					input = arg;
				}
			} else if(strcmp(option, "pg") == 0) {
				set_be_option("gprof");
				obstack_printf(&ldflags_obst, " -pg");
			} else if(strcmp(option, "pedantic") == 0) {
				fprintf(stderr, "warning: ignoring gcc option '%s'\n", arg);
			} else if(strncmp(option, "std=", 4) == 0) {
				if(strcmp(&option[4], "c99") == 0) {
					c_mode = _C89|_C99;
				} else if(strcmp(&option[4], "c89") == 0) {
					c_mode = _C89;
				} else if(strcmp(&option[4], "gnu99") == 0) {
					c_mode = _C89|_C99|_GNUC;
				} else if(strcmp(&option[4], "microsoft") == 0) {
					c_mode = _C89|_C99|_MS;
				} else
					fprintf(stderr, "warning: ignoring gcc option '%s'\n", arg);
			} else if(strcmp(option, "version") == 0) {
				print_cparser_version();
			} else if (option[0] == '-') {
				/* double dash option */
				++option;
				if(strcmp(option, "gcc") == 0) {
					c_mode |= _GNUC;
				} else if(strcmp(option, "no-gcc") == 0) {
					c_mode &= ~_GNUC;
				} else if(strcmp(option, "ms") == 0) {
					c_mode |= _MS;
				} else if(strcmp(option, "no-ms") == 0) {
					c_mode &= ~_MS;
				} else if(strcmp(option, "signed-chars") == 0) {
					char_is_signed = true;
				} else if(strcmp(option, "unsigned-chars") == 0) {
					char_is_signed = false;
				} else if(strcmp(option, "strict") == 0) {
					strict_mode = true;
				} else if(strcmp(option, "lextest") == 0) {
					mode = LexTest;
				} else if(strcmp(option, "benchmark") == 0) {
					mode = BenchmarkParser;
				} else if(strcmp(option, "print-ast") == 0) {
					mode = PrintAst;
				} else if(strcmp(option, "print-implicit-cast") == 0) {
					print_implicit_casts = true;
				} else if(strcmp(option, "print-parenthesis") == 0) {
					print_parenthesis = true;
				} else if(strcmp(option, "print-fluffy") == 0) {
					mode = PrintFluffy;
				} else if(strcmp(option, "print-caml") == 0) {
					mode = PrintCaml;
				} else if(strcmp(option, "version") == 0) {
					print_cparser_version();
					exit(EXIT_SUCCESS);
				} else if(strcmp(option, "dump-function") == 0) {
					++i;
					if(i >= argc) {
						fprintf(stderr, "error: "
						        "expected argument after '--dump-function'\n");
						argument_errors = true;
						break;
					}
					dumpfunction = argv[i];
					mode         = CompileDump;
				} else {
					fprintf(stderr, "error: unknown argument '%s'\n", arg);
					argument_errors = true;
				}
			} else {
				fprintf(stderr, "error: unknown argument '%s'\n", arg);
				argument_errors = true;
			}
		} else {

			file_list_entry_t *entry
				= obstack_alloc(&file_obst, sizeof(entry[0]));
			entry->filename = arg;

			size_t len = strlen(arg);
			if (len < 2) {
				if (arg[0] == '-') {
					/* exception: '-' as name reads C from stdin */
					entry->next = c_files;
					c_files     = entry;
					continue;
				}
				fprintf(stderr, "'%s': file format not recognized\n", input);
				continue;
			}

			if (strcmp(arg+len-2, ".c") == 0
					|| strcmp(arg+len-2, ".h") == 0) {
				entry->next = c_files;
				c_files     = entry;
			} else if (strcmp(arg+len-2, ".s") == 0) {
				entry->next = s_files;
				s_files     = entry;
			} else if (strcmp(arg+len-2, ".o") == 0) {
				entry->next = o_files;
				o_files     = entry;
			} else {
				fprintf(stderr, "'%s': file format not recognized\n", input);
			}
		}
	}

	if (c_files == NULL && s_files == NULL && o_files != NULL) {
		mode = Link;
	} else if (c_files != NULL && c_files->next == NULL) {
		input = c_files->filename;
	} else {
		if (c_files == NULL) {
			fprintf(stderr, "error: no input files specified\n");
		} else {
			fprintf(stderr, "error: multiple input files specified\n");
		}
		argument_errors = true;
	}

	/* we do the lowering in ast2firm */
	firm_opt.lower_bitfields = FALSE;

	if (help_displayed) {
		return !argument_errors;
	}
	if (argument_errors) {
		usage(argv[0]);
		return 1;
	}

	gen_firm_init();
	init_symbol_table();
	init_tokens();
	init_types();
	init_typehash();
	init_basic_types();
	init_lexer();
	init_ast();
	init_parser();
	init_ast2firm();

	FILE *out = NULL;
	char  outnamebuf[4096];
	if(outname == NULL) {
		switch(mode) {
		case BenchmarkParser:
		case PrintAst:
		case PrintFluffy:
		case PrintCaml:
		case LexTest:
			if(outname == NULL)
				outname = "-";
			break;
		case PreprocessOnly:
			break;
		case ParseOnly:
			break;
		case Compile:
			get_output_name(outnamebuf, sizeof(outnamebuf), input, ".s");
			outname = outnamebuf;
			break;
		case CompileAssemble:
			get_output_name(outnamebuf, sizeof(outnamebuf), input, ".o");
			outname = outnamebuf;
			break;
		case CompileDump:
			get_output_name(outnamebuf, sizeof(outnamebuf), dumpfunction,
			                ".vcg");
			outname = outnamebuf;
			break;
		case Link:
		case CompileAssembleLink:
#ifdef _WIN32
			/* Windows compiler typically derive the output name from
			   the first source file */
			get_output_name(outnamebuf, sizeof(outnamebuf), input, ".exe");
			outname = outnamebuf;
#else
			outname = "a.out";
#endif
			break;
		}
	}

	if (mode == Link) {
		obstack_1grow(&ldflags_obst, '\0');
		const char *flags = obstack_finish(&ldflags_obst);

		obstack_printf(&file_obst, "%s", LINKER);

		for (file_list_entry_t *entry = o_files; entry != NULL;
				entry = entry->next) {
			obstack_printf(&file_obst, " %s", entry->filename);
		}

		obstack_printf(&file_obst, " -o %s %s", outname, flags);

		char *buf = obstack_finish(&file_obst);

		if(verbose) {
			puts(buf);
		}
		int err = system(buf);
		if(err != 0) {
			fprintf(stderr, "linker reported an error\n");
			exit(1);
		}
		return 0;
	}

	if(outname != NULL) {
		if(strcmp(outname, "-") == 0) {
			out = stdout;
		} else {
			out = fopen(outname, "w");
			if(out == NULL) {
				fprintf(stderr, "Couldn't open '%s' for writing: %s\n", outname,
				        strerror(errno));
				return 1;
			}
		}
	}

	FILE *in;
	if(input == NULL) {
		fprintf(stderr, "%s: no input files\n", argv[0]);
		return 1;
	} else if(strcmp(input, "-") == 0) {
		in    = stdin;
		input = "<stdin>";
	} else {
		in = fopen(input, "r");
		if(in == NULL) {
			fprintf(stderr, "Couldn't open '%s': %s\n", input, strerror(errno));
			return 1;
		}
	}

	if(mode == LexTest) {
		lextest(in, input);
		fclose(in);
		return 0;
	}

	FILE *preprocessed_in = preprocess(in, input, mode == PreprocessOnly);
	translation_unit_t *const unit = do_parsing(preprocessed_in, input);
	int res = pclose(preprocessed_in);
	if(res != 0) {
		return res;
	}

	if(error_count > 0) {
		/* parsing failed because of errors */
		fprintf(stderr, "%u error(s), %u warning(s)\n", error_count, warning_count);
		result = EXIT_FAILURE;
	} else if(warning_count > 0) {
		fprintf(stderr, "%u warning(s)\n", warning_count);
	}

	if(mode == BenchmarkParser) {
		return result;
	}

	/* prints the AST even if errors occurred */
	if(mode == PrintAst) {
		type_set_output(out);
		ast_set_output(out);
		print_ast(unit);
		return result;
	}

	/* cannot handle other modes with errors */
	if(result != EXIT_SUCCESS)
		return result;

	if(mode == PrintFluffy) {
		write_fluffy_decls(out, unit);
	}
	if (mode == PrintCaml) {
		write_caml_decls(out, unit);
	}

	translation_unit_to_firm(unit);

	if(mode == ParseOnly) {
		return 0;
	}

	FILE *asm_out;
	char  asm_tempfile[1024];
	if(mode == CompileDump) {
		asm_out = NULL;
		firm_be_opt.selection = BE_NONE;
	} else if(mode == Compile) {
		asm_out = out;
	} else {
		asm_out
			= make_temp_file(asm_tempfile, sizeof(asm_tempfile), "cc", ".s");
	}
	gen_firm_finish(asm_out, input, /*c_mode=*/1, /*firm_const_exists=*/0);

	if(mode == CompileDump) {
		/* find irg */
		ident    *id     = new_id_from_str(dumpfunction);
		ir_graph *irg    = NULL;
		int       n_irgs = get_irp_n_irgs();
		for(int i = 0; i < n_irgs; ++i) {
			ir_graph *tirg   = get_irp_irg(i);
			ident    *irg_id = get_entity_ident(get_irg_entity(tirg));
			if(irg_id == id) {
				irg = tirg;
				break;
			}
		}

		if(irg == NULL) {
			fprintf(stderr, "No graph for function '%s' found\n", dumpfunction);
			return 1;
		}

		dump_ir_block_graph_file(irg, out);
		fclose(out);
		return 0;
	}

	fclose(asm_out);

	/* assemble assembler and create object file */
	char obj_tfile[1024];
	if(mode == CompileAssemble || mode == CompileAssembleLink) {
		const char *obj_outfile;
		if(mode == CompileAssemble) {
			fclose(out);
			obj_outfile = outname;
		} else {
			FILE *tempf
				= make_temp_file(obj_tfile, sizeof(obj_tfile), "cc", ".o");
			fclose(tempf);
			obj_outfile = obj_tfile;
		}

		assemble(obj_outfile, asm_tempfile);
	}

	/* link object file */
	if(mode == CompileAssembleLink) {
		do_link(outname, obj_tfile);
	}

	obstack_free(&cppflags_obst, NULL);
	obstack_free(&ldflags_obst, NULL);
	obstack_free(&file_obst, NULL);

	exit_ast2firm();
	exit_parser();
	exit_ast();
	exit_lexer();
	exit_typehash();
	exit_types();
	exit_tokens();
	exit_symbol_table();
	return 0;
}
