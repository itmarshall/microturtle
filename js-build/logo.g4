/*
Logo grammar file for the "micro turtle" robot project.

Written by: Ian Marshall
 */

grammar logo;

/* The "program" is the root of a Logo program. */
program
    : (statement | comment)* EOF
    ;

/* A statement is an instruction or the definition of a procedure. */
statement
    : command
    | procedureDef
    ;

/* Each command is a single instruction, which may itself contain more instructions. */
command
    : FD expr                          # FD
    | BK expr                          # BK
    | LT expr                          # LT
    | RT expr                          # RT
    | PU                               # PU
    | PD                               # PD
    | RETURN                           # Return
    | STOP                             # Stop
    | MAKE '"' STRING expr             # Make
    | REPEAT expr block                # Repeat
    | IF condition block (ELSE block)? # If
    | procedureInvoke                  # InvokeProc
    ; 

/* The definition of a procedure that may be called by the program. */
procedureDef
    : TO STRING paramDef* (command | comment)* END
    ;

/* The definition of a parameter for a procedure. */
paramDef
    : ':' STRING
    ;

/* Invocation of a procedure, where it is called by the program. */
procedureInvoke
    : STRING expr*
    ;

/* A block is a group of one or more commands surrounded by square brackets. */
block
    : '[' comment? command (command | comment)* ']'
    ;

/* Condition used in one or more comparisons yielding a boolean (true/false) result. */
condition
    : '(' expr comparison expr ')'       # Comp
    | '(' condition (AND condition)+ ')' # Combine
    | '(' condition (OR condition)+ ')'  # Combine
    ;

/* Comparison operators. */
comparison
    : '='   # Eq
    | '!='  # NotEq
    | '<'   # LessThan
    | '<='  # LessThanEq
    | '>'   # GreaterThan
    | '>='  # GreaterThanEq
    ;

/* A single line string comment. Ignored. */
comment
    : COMMENT
    ;

/* Numeric expression. */
expr
    : SUB expr                 # Negate
    | expr (MUL | DIV) expr    # MulDiv
    | expr (ADD | SUB) expr    # AddSub
    | ':' STRING               # Deref
    | INTNUM                   # Int
    | '(' expr ')'             # Parens
    ;


/* Case insensitive token for the "forward" command. */
FD
    : [Ff][Dd]
    | [Ff][Oo][Ww][Aa][Rr][Dd]
    ;

/* Case insensitive token for the "back" command. */
BK
    : [Bb][Kk]
    | [Bb][Aa][Cc][Kk]
    ;

/* Case insensitive token for the "left" command. */
LT
    : [Ll][Tt]
    | [Ll][Ee][Ff][Tt]
    ;

/* Case insensitive token for the "right" command. */
RT
    : [Rr][Tt]
    | [Rr][Ii][Gg][Hh][Tt]
    ;

/* Case insensitive token for the "penup" command. */
PU
    : [Pp][Uu]
    | [Pp][Ee][Nn][Uu][Pp]
    ;

/* Case insensitive token for the "pendown" command. */
PD
    : [Pp][Dd]
    | [Pp][Ee][Nn][Dd][Oo][Ww][Nn]
    ;

/* Case insensitive token for the "make" command. */
MAKE
    : [Mm][Aa][Kk][Ee]
    ;

/* Case insensitive token for the "repeat" command. */
REPEAT
    : [Rr][Ee][Pp][Ee][Aa][Tt]
    ;

/* Case insensitive token for the "if" command. */
IF
    : [Ii][Ff]
    ;

/* Case insensitive token for the "else" clause of the "if" command. */
ELSE
    : [Ee][Ll][Ss][Ee]
    ;

/* Case insensitive token for the "penup" command. */
STOP
    : [Ss][Tt][Oo][Pp]
    ;

RETURN
    : [Rr][Ee][Tt][Uu][Rr][Nn]
    ;

TO
    : [Tt][Oo]
    ;

END
    : [Ee][Nn][Dd]
    ;

AND
    : [Aa][Nn][Dd]
    ;

OR
    : [Oo][Rr]
    ;

MUL
    : '*'
    ;

DIV
    : '/'
    ;

ADD
    : '+'
    ;

SUB
    : '-'
    ;

COMMENT
    : ';' ~ [\r\n]*
    ;

STRING
    : [A-Za-z][A-Za-z0-9_]*
    ;

INTNUM
    : '-'? [0-9]+
    ;

WS
    : [ \t\r\n] -> skip
    ;
