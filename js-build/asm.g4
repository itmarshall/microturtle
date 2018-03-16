/*
Assembler grammer for "micro turtle" bytecode.

Written by: Ian Marshall
 */

grammar asm;

/* The "program" is the root of an assembley program. */
program
    : GLOBALS NUM EOL line* EOF
    ;

/* Each line can be either a function definition, label or instruction, with 
 * or without a comment. */
line
    : DEF (ID|MAINID) ':' ARGS '=' NUM ',' LOCALS '=' NUM ',' STACK '=' NUM EOL # Def
    | ID ':' EOL                                                                # Label
    | instr comment? EOL                                                        # InstrLine
	| comment EOL                                                               # AsmComment
    ;

/* A single assembly instruction - we only have to handle branches and function
 * calls separately, the rest are lumped together. */
instr
    : BRANCH ID # Branch
    | CALL ID   # Call
    | ID NUM?   # OtherInstr
    ;

/* Single line string comment. Ignored. */
comment
	: COMMENT
	;

GLOBALS
    : '.globals'
    ;

DEF
    : '.def'
    ;

ARGS
    : 'args'
    ;

LOCALS
    : 'locals'
    ;

STACK
	: 'stack'
	;

BRANCH
    : 'br' [tf]?
    ;

CALL
    : 'call'
    ;

MAINID
    : '<main>'
    ;

ID
    : [a-zA-Z][a-zA-Z0-9_-]*
    ;

NUM
    : [0-9]+
    ;

COMMENT
	: ';' ~ [\r\n]*
	;

EOL
    : '\r'?'\n'
    ;

WS
    : [ \t] -> skip
    ;
