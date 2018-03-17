var antlr4 = require('antlr4/index');
var logoLexer = require('./logoLexer');
var logoParser = require('./logoParser');
var logoListener = require('./logoListener').logoListener;
var asmLexer = require('./asmLexer');
var asmParser = require('./asmParser');
var asmListener = require('./asmListener').asmListener;
var errorListener = antlr4.error.ErrorListener;

/*
 * Scope objects are used to store variables and functions (separate name 
 * spaces) in the different program scopes, and look them up later.
 */
function Scope(parent) {
    this._parent = parent;
    this._variables = new Map();
    this._procedures = new Map();
	this._currentStackDepth = 0;
	this._maxStackDepth = 0;
    return this;
}
/*
 * Resolves a variable in the local scope only, or returns undefined if no
 * variable can be found in this scope.
 */
Scope.prototype.resolveLocalVariable = function(name) {
    return this._variables.get(name);
};
/*
 * Resolves a procedure in the local scope only, or returns undefined if no
 * procedure can be found in this scope.
 */
Scope.prototype.resolveLocalProcedure = function(name) {
    return this._procedures.get(name);
};
/*
 * Resolves a variable in the local scope, and any parent scopes, or returns 
 * undefined if no variable can be found in this scope hierarchy.
 */
Scope.prototype.resolveVariable = function(name) {
    // Find the name in the symbol table for this scope.
    var result = this.resolveLocalVariable(name);

    if (result !== undefined) {
        // Return the found symbol.
        return result;
    } else {
        // Nothing was found, check up the chain.
        if (this._parent !== undefined) {
            // Check the parent scope (and any higher up parents too).
            return this._parent.resolveVariable(name);
        } else {
            // There's no parent, so the item doesn't exist.
            return undefined;
        }
    }
};
/*
 * Resolves a procedure in the local scope, and any parent scopes, or returns 
 * undefined if no procedure can be found in this scope hierarchy.
 */
Scope.prototype.resolveProcedure = function(name) {
    // Find the name in the symbol table for this scope.
    var result = this.resolveLocalProcedure(name);

    if (result !== undefined) {
        // Return the found symbol.
        return result;
    } else {
        // Nothing was found, check up the chain.
        if (this._parent !== undefined) {
            // Check the parent scope (and any higher up parents too).
            return this._parent.resolveProcedure(name);
        } else {
            // There's no parent, so the item doesn't exist.
            return undefined;
        }
    }
};
/*
 * Defines a variable in the current scope, returning true if the variable 
 * didn't already exist in this scope when this function was called, false if 
 * it did.
 */
Scope.prototype.defineVariable = function(name) {
    if (this.resolveLocalVariable(name) !== undefined) {
        return false;
    } else {
        this._variables.set(name, {type: "variable", index: this._variables.size});
        return true;
    }
};
/*
 * Defines a procedure in the current scope, returning true if the procedure 
 * didn't already exist in this scope when this function was called, false if 
 * it did.
 */
Scope.prototype.defineProcedure = function(name, paramCount, scope) {
    if (this.resolveLocalProcedure(name) !== undefined) {
        return false;
    } else {
        this._procedures.set(name, 
                {type: "procedure", paramCount: paramCount, scope: scope});
        return true;
    }
};
/*
 * Increments the stack usage amount in the current scope. It is incremented by
 * the amount, but the maximum judged by the sum of amount and boost, which is
 * the temporary peak stack utilisation.
 */
Scope.prototype.incrementStack = function(amount, boost) {
	this._currentStackDepth += amount;
	this._maxStackDepth = Math.max(
			this._maxStackDepth, this._currentStackDepth + boost);
};
/*
 * Decrements the stack usage amount in the current scope. It is reduced by
 * the amount, but the maximum judged by increasing the prior value by the boost
 * before removing the amount to account for temporary peak stack utilisation.
 */
Scope.prototype.decrementStack = function(amount, boost) {
	this._maxStackDepth = Math.max(
			this._maxStackDepth, this._currentStackDepth + boost);
	this._currentStackDepth -= amount;
};
/*
 * Returns the maximum stack size required for this scope's operations.
 */
Scope.prototype.getStackSize = function() {
	return this._maxStackDepth;
};
/*
 * Returns the number of variables defined in this scope.
 */
Scope.prototype.getVariableCount = function() {
    return this._variables.size;
};
/*
 * Defines the name of an anonymous variable for a given depth/instance. The
 * variable itself is not stored in this scope.
 */
Scope.prototype.nameAnonymousVariable = function(depth, instance) {
    var name = "__Anon-" + depth + "-" + instance;
    return name;
};

/* 
 * The AssemblerFactory object is used to create assembly language code. This is
 * an intermediate representation in the Logo -> assembly -> bytecode 
 * compilation process.
 */
function AssemblerFactory() {
    this._procedures = [undefined];
    this._procedureMap = new Map();
    this._instructionMap = new Map();

    this._segments = [];
    this._segmentIndex = -1;
    this._currentSegment = undefined;
    return this;
};
/*
 * Pushes a segment of code onto the stack of segments. A segment can be used to
 * hold assembly code for various parts of the output while the code is still 
 * being parsed.
 */
AssemblerFactory.prototype.pushSegment = function() {
    this._currentSegment = [];
    this._segmentIndex = this._segments.length;
    this._segments.push(this._currentSegment);
}
/*
 * Pops a segment from the stack of segments and returns its contents.
 */
AssemblerFactory.prototype.popSegment = function() {
    this._segmentIndex--;
    if (this._segmentIndex < 0) {
        this._currentSegment = undefined;
    } else {
        this._currentSegment = this._segments[this._segmentIndex];
    }
    return this._segments.pop();
};
/*
 * Inserts a segment's instructions to the end of the current segment.
 */
AssemblerFactory.prototype.insertSegment = function(segment) {
    for (var ii = 0; ii < segment.length; ii++) {
        this._currentSegment.push(segment[ii]);
    }
};
/*
 * Adds a new procedure definition.
 */
AssemblerFactory.prototype.addProcedure = function(
        name, paramCount, variableCount, stackSize) {
    var proc = {name:          name, 
                paramCount:    paramCount,
                variableCount: variableCount,
				stackSize:     stackSize};
    if (name === undefined) {
        // The undefined name is the implicit "main" procedure.
        this._procedures[0] = proc;
        this._procedureMap.set(undefined, 0);
        this._currentSegment.push(".def <main>: args=0, locals=" +
                proc.variableCount + ", stack=" + proc.stackSize);
    } else {
        // This is an ordinary procedure.
        var index = this._procedures.length;
        this._procedures.push(proc);
        this._procedureMap.set(name, index);
        this._currentSegment.push(".def " + name + 
				": args=" + proc.paramCount + 
				", locals=" + proc.variableCount +
				", stack=" + proc.stackSize);
    }
};
/*
 * Assigns a segment to a procedure as its set of instructions. Any previous
 * instructions are removed.
 */
AssemblerFactory.prototype.assignSegmentToProcedure = function(name, segment) {
    this._instructionMap.set(name, segment);
};
/*
 * Retrieves the segments for a supplied procedure.
 */
AssemblerFactory.prototype.getProcedureSegment = function(name) {
    return this._instructionMap.get(name);
};
/*
 * Gets the list of defined procedure names.
 */
AssemblerFactory.prototype.getProcedures = function() {
    var names = [];
    //for (let key of this._procedureMap.keys()) {
	var key;
    for (key of this._procedureMap.keys()) {
        names.push(key);
    }
    return names;
};

/* --- Individual instruction generation functions below --- */
AssemblerFactory.prototype.instrFd = function() {
    this._currentSegment.push("  fd");
};
AssemblerFactory.prototype.instrBk = function() {
    this._currentSegment.push("  bk");
};
AssemblerFactory.prototype.instrLt = function() {
    this._currentSegment.push("  lt");
}
AssemblerFactory.prototype.instrRt = function() {
    this._currentSegment.push("  rt");
};
AssemblerFactory.prototype.instrPu = function() {
    this._currentSegment.push("  pu");
};
AssemblerFactory.prototype.instrPd = function() {
    this._currentSegment.push("  pd");
};
AssemblerFactory.prototype.instrIAdd = function() {
    this._currentSegment.push("  iadd");
};
AssemblerFactory.prototype.instrISub = function() {
    this._currentSegment.push("  isub");
};
AssemblerFactory.prototype.instrIMul = function() {
    this._currentSegment.push("  imul");
};
AssemblerFactory.prototype.instrIDiv = function() {
    this._currentSegment.push("  idiv");
};
AssemblerFactory.prototype.instrIConst = function(val) {
    switch (val) {
        case 0:
            this._currentSegment.push("  iconst_0");
            break;
        case 1:
            this._currentSegment.push("  iconst_1");
            break;
        case 45:
            this._currentSegment.push("  iconst_45");
            break;
        case 90:
            this._currentSegment.push("  iconst_90");
            break;
        default:
            this._currentSegment.push("  iconst  " + val);
            break;
    }
};
AssemblerFactory.prototype.instrILoad = function(index) {
    switch (index) {
        case 0:
            this._currentSegment.push("  iload_0");
            break;
        case 1:
            this._currentSegment.push("  iload_1");
            break;
        case 2:
            this._currentSegment.push("  iload_2");
            break;
        default:
            this._currentSegment.push("  iload  " + index);
            break;
    }
};
AssemblerFactory.prototype.instrIStore = function(index) {
    switch (index) {
        case 0:
            this._currentSegment.push("  istore_0");
            break;
        case 1:
            this._currentSegment.push("  istore_1");
            break;
        case 2:
            this._currentSegment.push("  istore_2");
            break;
        default:
            this._currentSegment.push("  istore  " + index);
            break;
    }
};
AssemblerFactory.prototype.instrGLoad = function(index) {
    switch (index) {
        case 0:
            this._currentSegment.push("  gload_0");
            break;
        case 1:
            this._currentSegment.push("  gload_1");
            break;
        case 2:
            this._currentSegment.push("  gload_2");
            break;
        default:
            this._currentSegment.push("  gload  " + index);
            break;
    }
};
AssemblerFactory.prototype.instrGStore = function(index) {
    switch (index) {
        case 0:
            this._currentSegment.push("  gstore_0");
            break;
        case 1:
            this._currentSegment.push("  gstore_1");
            break;
        case 2:
            this._currentSegment.push("  gstore_2");
            break;
        default:
            this._currentSegment.push("  gstore  " + index);
            break;
    }
};
AssemblerFactory.prototype.instrILt = function() {
    this._currentSegment.push("  ilt");
};
AssemblerFactory.prototype.instrILe = function() {
    this._currentSegment.push("  ile");
};
AssemblerFactory.prototype.instrIGt = function() {
    this._currentSegment.push("  igt");
};
AssemblerFactory.prototype.instrIGe = function() {
    this._currentSegment.push("  ige");
};
AssemblerFactory.prototype.instrIEq = function() {
    this._currentSegment.push("  ieq");
};
AssemblerFactory.prototype.instrINe = function() {
    this._currentSegment.push("  ine");
};
AssemblerFactory.prototype.label = function(label) {
    this._currentSegment.push(label + ":");
};
AssemblerFactory.prototype.instrCall = function(name) {
    this._currentSegment.push("  call  " + name);
};
AssemblerFactory.prototype.instrRet = function() {
    this._currentSegment.push("  ret");
};
AssemblerFactory.prototype.instrStop = function() {
    this._currentSegment.push("  stop");
};
AssemblerFactory.prototype.instrBr = function(label) {
    this._currentSegment.push("  br  " + label);
};
AssemblerFactory.prototype.instrBrt = function(label) {
    this._currentSegment.push("  brt  " + label);
};
AssemblerFactory.prototype.instrBrf = function(label) {
    this._currentSegment.push("  brf  " + label);
};

/*
 * The LogoDefs object is used to record the definitions of variables and 
 * procedures. This is used to ensure variable names are referenced correctly,
 * and to store procedure names and parameter counts for later checks.
 */
function LogoDefs(exceptions) {
    logoListener.call(this);
    this._scopes = undefined;
    this._globalScope = undefined;
    this._currentScope = undefined;
    this._exceptions = exceptions;
    this._repeatDepth = 0;
    return this;
};
LogoDefs.prototype = Object.create(logoListener.prototype);
LogoDefs.prototype.constructor = LogoDefs;
/* 
 * Retrieves the global scope, that is the code/variables outside of a defined 
 * procedure.
 */
LogoDefs.prototype.getGlobalScope = function() {
    return this._globalScope;
};
/*
 * Retrieves the map of scopes used to hold definitions across all of the 
 * program's scopes.
 */
LogoDefs.prototype.getScopes = function() {
    return this._scopes;
};
/*
 * Handles the lexer's entry into a program by initialising data.
 */
LogoDefs.prototype.enterProgram = function(ctx) {
    this._globalScope = new Scope(undefined);
    var scope = new Scope(this._globalScope);
    this._scopes = new Map();
    this._scopes.set(undefined, scope);
    this._currentScope = scope;
};
/*
 * Processes the lexer entering a procedure definition by recording the
 * procedure, and handling the scope hierarchy.
 */
LogoDefs.prototype.enterProcedureDef = function(ctx) {
    var name = ctx.STRING().getText();
    var paramCount = ctx.paramDef().length;
    if (!this._currentScope.defineProcedure(name, paramCount, 
                this._currentScope)) {
        var start = ctx.start;
        var col = start.column + ctx.TO().getText().length + 1;
        this._exceptions.push({line: start.line, col: col,
                message: "Duplicate procedure name \"" +
                ctx.STRING().getText() + "\"."});
    }
    var scope = new Scope(this._currentScope);
    this._scopes.set(name, scope);
    this._currentScope = scope;
};
/*
 * Processes the lexer exiting a procedure definition to ensure the scope 
 * hierarchy stays correct to the current procedure.
 */
LogoDefs.prototype.exitProcedureDef = function(ctx) {
    this._currentScope = this._currentScope._parent;
};
/*
 * Processes the lexer exiting a parameter definition by storing the variable 
 * definition.
 */
LogoDefs.prototype.exitParamDef = function(ctx) {
    this._currentScope.defineVariable(ctx.STRING().getText())
};
/*
 * Processes the lexer exiting a make, which is a variable definition.
 */
LogoDefs.prototype.exitMake = function(ctx) {
    var name = ctx.STRING();
    if (name !== null) {
        name = name.getText();
    } else {
        name = "<undefined>";
    }
    if (this._currentScope._parent == this._globalScope) {
        // This is the main function, variables here are global.
        this._globalScope.defineVariable(name)
    } else {
        this._currentScope.defineVariable(name)
    }
};
/*
 * Processes the lexer exiting a variable dereference to ensure all variable 
 * references point to a valid variable at this point in the program.
 */
LogoDefs.prototype.exitDeref = function(ctx) {
    var name = ctx.STRING().getText();
    if (this._currentScope.resolveVariable(name) === undefined) {
        var start = ctx.start;
        this._exceptions.push({line: start.line, col: start.column + 1,
                message: "Unknown variable \"" + name + "\"."});
    }
};
/*
 * Processes the lexer entering a repeat section, which requires anonymous 
 * variables to control the loop's execution.
 */
LogoDefs.prototype.enterRepeat = function(ctx) {
    this._repeatDepth++;
    var name = this._currentScope.nameAnonymousVariable(this._repeatDepth, 0);
    this._currentScope.defineVariable(name);
    name = this._currentScope.nameAnonymousVariable(this._repeatDepth, 1);
    this._currentScope.defineVariable(name);
};
/*
 * Processes the lexer exiting a repeat section by keeping the repeat depth in 
 * sync with the lexer's tree walk.
 */
LogoDefs.prototype.exitRepeat = function(ctx) {
    this._repeatDepth--;
};

/*
 * The LogoRefs object is used to check function references so that all 
 * procedure calls are calling a valid procedure, with the right number of 
 * parameters. This needs to be performed in a separate step to account for 
 * forward references to procedures.
 */
function LogoRefs(scopes, exceptions) {
    logoListener.call(this);
    this._scopes = scopes;
    this._currentScope = undefined;
    this._exceptions = exceptions;
    return this;
};
LogoRefs.prototype = Object.create(logoListener.prototype);
LogoRefs.prototype.constructor = LogoRefs;
// TODO: Fill me in.
LogoRefs.prototype.incrementStack = function(ctx, boost=0) {
	this._currentScope.incrementStack(1, boost);
};
LogoRefs.prototype.decrementStack = function(ctx, amount=1, boost=0) {
	this._currentScope.decrementStack(amount, boost);
};
/*
 * Processes the lexer entering a program by initialising the scope.
 */
LogoRefs.prototype.enterProgram = function(ctx) {
    this._currentScope = this._scopes.get(undefined);
};
/*
 * Processes the lexer entering a procedure definition by updating the current 
 * scope.
 */
LogoRefs.prototype.enterProcedureDef = function(ctx) {
    var name = ctx.STRING().getText();
    this._currentScope = this._scopes.get(name);
};
/*
 * Processes the lexer exiting a procedure definition by updating the current
 * scope.
 */
LogoRefs.prototype.exitProcedureDef = function(ctx) {
    this._currentScope = this._currentScope._parent;
};
/*
 * Processes the lexer entering a procedure invocation by checking that it is a
 * valid invocation.
 */
LogoRefs.prototype.enterProcedureInvoke = function(ctx) {
    var name = ctx.STRING().getText();
    var proc = this._currentScope.resolveProcedure(name);
    if (proc === undefined) {
        var start = ctx.start;
        this._exceptions.push({line: start.line, col: start.column + 1,
                message: "Unknown procedure name \"" + name + "\"."});
    } else if (ctx.expr().length !== proc.paramCount) {
        var start = ctx.start;
        this._exceptions.push({line: start.line, col: start.column + 1,
                message: "Incorrect parameter count for procedure \"" + name +
                "\", " + ctx.expr().length + " instead of " + proc.paramCount + 
                "."});
    } else {
		// Update the stack count.
		this.decrementStack(ctx, proc.paramCount);
	}
};
LogoRefs.prototype.exitIf = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitFD = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitBK = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitLT = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitRT = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitMulDiv = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitAddSub = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitInt = function(ctx) {this.incrementStack(ctx)};
LogoRefs.prototype.exitDeref = function(ctx) {this.incrementStack(ctx)};
LogoRefs.prototype.exitComp = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitMake = function(ctx) {this.decrementStack(ctx)};
LogoRefs.prototype.exitRepeat = function(ctx) {
	this.decrementStack(ctx, 1, 1);
}
LogoRefs.prototype.exitNegate = function(ctx) {
	this.incrementStack(ctx, 2);
};

/*
 * The LogoAssembler object receives events from the lexer's walking of the
 * program tree, and creates the matching assembly instructions.
 */
function LogoAssembler(globalScope, scopes) {
    logoListener.call(this);
    this._scopes = scopes;
    this._globalScope = globalScope;
    this._currentScope = undefined;
    this._asm = new AssemblerFactory();
    this._frames = [];
    this._repeatDepth = 0;
    this._labelNumber = -1;
    return this;
};
LogoAssembler.prototype = Object.create(logoListener.prototype);
LogoAssembler.prototype.constructor = LogoAssembler;
/*
 * Retrieves all of the assembly instructions in text form, one instruction per
 * line.
 */
LogoAssembler.prototype.getText = function() {
    var text = ".globals " + this._globalScope.getVariableCount() + "\n";
    var procs = this._asm.getProcedures();
    for (var index in procs) {
        var segment = this._asm.getProcedureSegment(procs[index]);
        for (var ii = 0; ii < segment.length; ii++) {
            text += segment[ii] + "\n";
        }
    }

    return text;
};
/*
 * Retrieves the prefix string for the next label, used to ensure unique labels
 * are generated at all times.
 */
LogoAssembler.prototype.getNextLabelBase = function(text) {
    this._labelNumber++;
    return text + "-" + this._labelNumber;
};
/*
 * Processes the lexer entering the program by initialising this object's state.
 */
LogoAssembler.prototype.enterProgram = function(ctx) {
    this._currentScope = this._scopes.get(undefined);
    this._asm.pushSegment();
    this._asm.addProcedure(undefined, 0, this._currentScope.getVariableCount(),
			this._currentScope.getStackSize());
};
/*
 * Processes the lexer entering a procedure definition by generating the 
 * appropriate assembly code, and handling the correct scope and segment 
 * settings.
 */
LogoAssembler.prototype.enterProcedureDef = function(ctx) {
    var name = ctx.STRING().getText();
    this._currentScope = this._scopes.get(name);
    this._asm.pushSegment();
    this._asm.addProcedure(ctx.STRING().getText(), ctx.paramDef().length,
            this._currentScope.getVariableCount() - ctx.paramDef().length,
			this._currentScope.getStackSize());
};
/*
 * Processes the lexer exiting a procedure definition by generating a return 
 * instruction and keeping the scopes in sync.
 */
LogoAssembler.prototype.exitProcedureDef = function(ctx) {
    this._asm.instrRet();
    this._currentScope = this._currentScope._parent;
    this._asm.assignSegmentToProcedure(ctx.STRING().getText(),
            this._asm.popSegment());
};
/*
 * Processes the lexer exiting the program by generating a stop instruction
 * and storing the global code in the main procedure.
 */
LogoAssembler.prototype.exitProgram = function(ctx) {
    this._asm.instrStop();
    this._asm.assignSegmentToProcedure(undefined, this._asm.popSegment());
};
/*
 * Processes the lexer entering a repeat statement by preparing a new frame to 
 * hold the code segment for the repeat block.
 */
LogoAssembler.prototype.enterRepeat = function(ctx) {
    this._repeatDepth++;
    this._frames.push([]);
};
/*
 * Processes the lexer exiting a repeat statement by placing the correct 
 * assembly instructions in the correct order (including the repeat block).
 */
LogoAssembler.prototype.exitRepeat = function(ctx) {
    var segments = this._frames.pop();
    if (segments.length > 0) {
        var name = this._currentScope.nameAnonymousVariable(this._repeatDepth,
                0);
        var var1 = this._currentScope.resolveLocalVariable(name);
        this._asm.instrIStore(var1.index);
        this._asm.instrIConst(0);
        name = this._currentScope.nameAnonymousVariable(this._repeatDepth, 1);
        var var2 = this._currentScope.resolveLocalVariable(name);
        this._asm.instrIStore(var2.index);
        var labelBase = this.getNextLabelBase("repeat");
        this._asm.label(labelBase + "-start");
        this._asm.instrILoad(var2.index);
        this._asm.instrILoad(var1.index);
        this._asm.instrIGe();
        this._asm.instrBrt(labelBase + "-end");
        this._asm.insertSegment(segments[0]);
        this._asm.instrBr(labelBase + "-start");
        this._asm.label(labelBase + "-end");
    }

    this._repeatDepth--;
};
/*
 * Processes the lexer entering an if statement by preparing a new frame to 
 * hold the code segments for the "if" and "else" (optional) instructions.
 */
LogoAssembler.prototype.enterIf = function(ctx) {
    this._frames.push([]);
};
/*
 * Processes the lexer exiting an if statement by placing the correct 
 * assembly instructions in the correct order (including the "if" and "else" 
 * blocks).
 */
LogoAssembler.prototype.exitIf = function(ctx) {
    var segments = this._frames.pop();
    var labelBase = this.getNextLabelBase("if");
    this._asm.insertSegment(segments[0]);
    this._asm.instrBrf(labelBase + "-false");
    this._asm.insertSegment(segments[1]);
    if (ctx.ELSE() !== null) {
        this._asm.instrBr(labelBase + "-end");
        this._asm.label(labelBase + "-false");
        this._asm.insertSegment(segments[2]);
        this._asm.label(labelBase + "-end");
    } else {
        this._asm.label(labelBase + "-false");
    }
};
/*
 * Processes the lexer entering a block by preparing a segment to hold this
 * block's assembly code.
 */
LogoAssembler.prototype.enterBlock = function(ctx) {
    this._asm.pushSegment();
};
/*
 * Processes the lexer exiting a block by pushing the segment holding this 
 * block's code on to the current frame.
 */
LogoAssembler.prototype.exitBlock = function(ctx) {
    this._frames[this._frames.length - 1].push(this._asm.popSegment());
};
/*
 * Processes the lexer entering a boolean combine (and/or) operation by 
 * preparing a segment and frame for it.
 */
LogoAssembler.prototype.enterCombine = function(ctx) {
    this._asm.pushSegment();
    this._frames.push([]);
};
/*
 * Processes the lexer exiting a boolean combine (and/or) operation by inserting
 * the appropriate assembly code and labels to enable short-circuit evaluation
 * and the processing of the segments from the combination's frame.
 */
LogoAssembler.prototype.exitCombine = function(ctx) {
    var segments = this._frames.pop();
    this._asm.insertSegment(segments[0]);
    var labelBase = this.getNextLabelBase("cmp");
	var andOp = true;
	if (ctx.OR().length > 0) {
		andOp = false;
	}
	for (var ii = 1; ii < segments.length; ii++) {
		if (andOp) {
			// And.
			this._asm.instrBrf(labelBase + "-fail");
		} else {
			// Or.
			this._asm.instrBrt(labelBase + "-pass");
		}
		this._asm.insertSegment(segments[ii]);
	}
    this._asm.instrBr(labelBase + "-end");
    if (andOp) {
        this._asm.label(labelBase + "-fail");
        this._asm.instrIConst(0);
    } else {
        this._asm.label(labelBase + "-pass");
        this._asm.instrIConst(1);
    }
    this._asm.label(labelBase + "-end");
    this._frames[this._frames.length - 1].push(this._asm.popSegment());
};
/* --- Simple instruction generation functions below --- */
LogoAssembler.prototype.exitProcedureInvoke = function(ctx) {
    var name = ctx.STRING().getText();
    this._asm.instrCall(name);
};
LogoAssembler.prototype.exitFD = function(ctx) {
    this._asm.instrFd();
};
LogoAssembler.prototype.exitBK = function(ctx) {
    this._asm.instrBk();
};
LogoAssembler.prototype.exitLT = function(ctx) {
    this._asm.instrLt();
};
LogoAssembler.prototype.exitRT = function(ctx) {
    this._asm.instrRt();
};
LogoAssembler.prototype.exitPU = function(ctx) {
    this._asm.instrPu();
};
LogoAssembler.prototype.exitPD = function(ctx) {
    this._asm.instrPd();
};
LogoAssembler.prototype.exitMulDiv = function(ctx) {
    if (ctx.MUL() !== null) {
        this._asm.instrIMul();
    } else {
        this._asm.instrIDiv();
    }
};
LogoAssembler.prototype.exitAddSub = function(ctx) {
    if (ctx.ADD() !== null) {
        this._asm.instrIAdd();
    } else {
        this._asm.instrISub();
    }
};
LogoAssembler.prototype.exitInt = function(ctx) {
    this._asm.instrIConst(parseInt(ctx.INTNUM().getText()));
};
LogoAssembler.prototype.exitDeref = function(ctx) {
    var name = ctx.STRING().getText();
    var variable = this._currentScope.resolveLocalVariable(name);
    if (variable === undefined) {
        // We have to go to the globals for this one.
        variable = this._currentScope.resolveVariable(name);
        this._asm.instrGLoad(variable.index);
    } else {
        // Get the local variable.
        this._asm.instrILoad(variable.index);
    }
};
LogoAssembler.prototype.exitNegate = function(ctx) {
    this._asm.instrIConst(-1);
    this._asm.instrIMul();
};
LogoAssembler.prototype.exitMake = function(ctx) {
    var name = ctx.STRING().getText();
    var variable = this._currentScope.resolveLocalVariable(name);
    if (variable === undefined) {
        // We have to go to the globals for this one.
        variable = this._currentScope.resolveVariable(name);
        this._asm.instrGStore(variable.index);
    } else {
        // Get the local variable.
        this._asm.instrIStore(variable.index);
    }
};
LogoAssembler.prototype.enterComp = function(ctx) {
    this._asm.pushSegment();
};
LogoAssembler.prototype.exitComp = function(ctx) {
    switch (ctx.comparison().getText()) {
        case "=":
            this._asm.instrIEq();
            break;
        case "!=":
            this._asm.instrINe();
            break;
        case "<":
            this._asm.instrILt();
            break;
        case "<=":
            this._asm.instrILe();
            break;
        case ">":
            this._asm.instrIGt();
            break;
        case ">=":
            this._asm.instrIGe();
            break;
    }
    this._frames[this._frames.length - 1].push(this._asm.popSegment());
};
LogoAssembler.prototype.exitReturn = function(ctx) {
    this._asm.instrRet();
};
LogoAssembler.prototype.exitStop = function(ctx) {
    this._asm.instrStop();
};

/*
 * The AsmCompiler object receives events from the lexer's walking of the
 * assembly program tree, and converts the assembly instructions to bytecode 
 * equivalents.
 */
function AsmCompiler() {
    asmListener.call(this);
    this._errors = [];
    this._codes = [];
    this._globalCount = 0;
    this._nextFunctionId = 0;
    this._currentFunctionName = undefined;
    this._functions = new Map();
    this._labels = new Map();
    this._unresolvedFunctions = [];
    this._unresolvedLabels = [];

	// Array holding: [opcode, bytecode, operands] data for each instruction.
    this._instrCodes = [
        // Movement op-codes.
        ["fd", 1, 0], ["bk", 2, 0], ["lt", 3, 0], ["rt", 4, 0], ["pu", 5, 0], ["pd", 6, 0], 
        // Arithmetic op-codes.
        ["iadd", 7, 0], ["isub", 8, 0], ["imul", 9, 0], ["idiv", 10, 0], 
        // Constant op-codes.
        ["iconst_0", 11, 0], ["iconst_1", 12, 0], ["iconst_45", 13, 0], ["iconst_90", 14, 0], ["iconst", 15, 1],
        // Variable load/store op-codes.
        ["iload_0",  16, 0], ["iload_1",  17, 0], ["iload_2",  18, 0], ["iload",  19, 1],
        ["istore_0", 20, 0], ["istore_1", 21, 0], ["istore_2", 22, 0], ["istore", 23, 1],
        ["gload_0",  24, 0], ["gload_1",  25, 0], ["gload_2",  26, 0], ["gload",  27, 1],
        ["gstore_0", 28, 0], ["gstore_1", 29, 0], ["gstore_2", 30, 0], ["gstore", 31, 1],
        // Comparison op-codes.
        ["ilt", 32, 0], ["ile", 33, 0], ["igt", 34, 0], ["ige", 35, 0], ["ieq", 36, 0], ["ine", 37, 0],
        // Control flow op-codes.
        ["call", 38, 1], ["ret", 39, 0], ["stop", 40, 0], ["br", 41, 1], ["brt", 42, 1], ["brf", 43, 1]];

        // Populate the instruction map with the above op-codes.
        this._instrMap = new Map();
        for (var ii = 0; ii < this._instrCodes.length; ii++) {
            this._instrMap.set(this._instrCodes[ii][0], 
                    {opcode: this._instrCodes[ii][1], operands: this._instrCodes[ii][2]});
        }
}
AsmCompiler.prototype = Object.create(asmListener.prototype);
AsmCompiler.prototype.constructor = AsmCompiler;
/*
 * Retrieves the bytecode that has been compiled by this object.
 */
AsmCompiler.prototype.getBytecode = function() {
    var functions = [];
    for (var [name, value] of this._functions) {
        functions.push({args: value.args, locals: value.locals, 
				stack: value.stack, codes: value.codes});
    }
    return {globals: this._globalCount, functions: functions};
};
/*
 * Processes the lexer entering a program by creating the global information.
 */
AsmCompiler.prototype.enterProgram = function(ctx) {
    var globals = parseInt(ctx.NUM().getText());
    if (globals > 255) {
        var start = ctx.start;
        this._errors.push({line: start.line, col: start.column + 1,
                message: "Global count cannot exceed 255."});
    } else {
        this._globalCount = globals;
    }
};
/*
 * Processes the lexer entering a function definition.
 */
AsmCompiler.prototype.enterDef = function(ctx) {
    var name;
    if (ctx.ID() === null) {
        name = ctx.MAINID().getText();
    } else {
        name = ctx.ID().getText();
    }
    var id = this._nextFunctionId++;
    var args = parseInt(ctx.NUM()[0].getText());
    var locals = parseInt(ctx.NUM()[1].getText());
    var stack = parseInt(ctx.NUM()[2].getText());
    this._currentFunctionName = name;
    this._codes = [];
    this._labels = new Map();
    this._functions.set(name, {id: id, args: args, locals: locals, stack: stack,
            codes: this._codes, labels: this._labels});
};
/*
 * Processes the lexer entering a label. We record the label and it's index
 * within the current function's code array for future reference.
 */
AsmCompiler.prototype.enterLabel = function(ctx) {
    var name = ctx.ID().getText();
    this._labels.set(name, this._codes.length);
};
/*
 * Processes the lexer entering a branch instruction. We store the branch 
 * location if we have already seen the label, otherwise we must record this 
 * point for filling in once the program's been parsed.
 */
AsmCompiler.prototype.enterBranch = function(ctx) {
    var instr = ctx.BRANCH().getText();
    var label = ctx.ID().getText();
    var op = this._instrMap.get(instr);
    if (op === undefined) {
        var start = ctx.start;
        this._errors.push({line: start.line, col: start.column + 1,
                message: "Unknown instruction: \"" + instr + "\"."});
    } else {
        this._codes.push(op.opcode);
        var labelIndex = this._labels.get(label);
        if (labelIndex === undefined) {
            // We can't resolve this label yet, remember where it should go, 
            // and store zeros for now.
            var start = ctx.start;
            this._unresolvedLabels.push(
                    {name:   label,
                     codes:  this._codes,
                     index:  this._codes.length,
                     fnName: this._currentFunctionName,
                     line:   start.line,
                     col:    start.column + 1});
            this._codes.push(0);
            this._codes.push(0);
            this._codes.push(0);
            this._codes.push(0);
        } else {
            this._codes.push((labelIndex & 0xFF000000) >> 24);
            this._codes.push((labelIndex & 0x00FF0000) >> 16);
            this._codes.push((labelIndex & 0x0000FF00) >> 8);
            this._codes.push((labelIndex & 0x000000FF));
        }
    }
};
/*
 * Processes the lexer entering a function call statement
 * Processes the lexer entering a function call instruction. We store the 
 * function ID if we have already seen the function, otherwise we must record 
 * this point for filling in once the program's been parsed.
 */
AsmCompiler.prototype.enterCall = function(ctx) {
    var instr = ctx.CALL().getText();
    var fnName = ctx.ID().getText();
    var op = this._instrMap.get(instr);
    if (op === undefined) {
        var start = ctx.start;
        this._errors.push({line: start.line, col: start.column + 1,
                message: "Unknown instruction: \"" + instr + "\"."});
    } else {
        this._codes.push(op.opcode);
        var fnInfo = this._functions.get(fnName);
        if (fnInfo === undefined) {
            // We can't resolve this function yet, remember where it should go,
            // and store zero.
            var start = ctx.start;
            this._unresolvedFunctions.push(
                    {name:  fnName, 
                     codes: this._codes,
                     index: this._codes.length,
                     line:  start.line,
                     col:   start.column + 1});
            this._codes.push(0);
            this._codes.push(0);
            this._codes.push(0);
            this._codes.push(0);
        } else {
            this._codes.push((fnInfo.id & 0xFF000000) >> 24);
            this._codes.push((fnInfo.id & 0x00FF0000) >> 16);
            this._codes.push((fnInfo.id & 0x0000FF00) >> 8);
            this._codes.push((fnInfo.id & 0x000000FF));
        }
    }
};
/*
 * Processes the lexer entering any other assembly instruction by emmitting the
 * appropriate bytecode value(s).
 */
AsmCompiler.prototype.enterOtherInstr = function(ctx) {
    var instr = ctx.ID().getText();
    var op = this._instrMap.get(instr);
    if (op === undefined) {
        var start = ctx.start;
        this._errors.push({line: start.line, col: start.column + 1,
                message: "Unknown instruction: \"" + instr + "\"."});
    } else {
        this._codes.push(op.opcode);
        if (op.operands == 1) {
            var num = parseInt(ctx.NUM().getText());
            this._codes.push((num & 0xFF000000) >> 24);
            this._codes.push((num & 0x00FF0000) >> 16);
            this._codes.push((num & 0x0000FF00) >> 8);
            this._codes.push((num & 0x000000FF));
        }
    }
};
/*
 * Processes the lexer exiting the program by going back through all unresolved
 * function and label references and filling them in.
 */
AsmCompiler.prototype.exitProgram = function(ctx) {
    // Update the unresolved function calls.
    for (var ii = 0; ii < this._unresolvedFunctions.length; ii++) {
        var item = this._unresolvedFunctions[ii];
        var fnInfo = this._functions.get(item.name);
        if (fnInfo === undefined) {
            // We still can't find the function, so it can't exist.
            this._errors.push({line: item.line, col: item.col,
                    message: "Unknown function: \"" + item.name + "\"."});
        } else {
            // Update this function call.
            item.codes[item.index]   = (labelIndex & 0xFF000000) >> 24;
            item.codes[item.index+1] = (labelIndex & 0x00FF0000) >> 16;
            item.codes[item.index+2] = (labelIndex & 0x0000FF00) >> 8;
            item.codes[item.index+3] = (labelIndex & 0x000000FF);
        }
    }

    // Update the unresolved label references.
    for (var ii = 0; ii < this._unresolvedLabels.length; ii++) {
        var item = this._unresolvedLabels[ii];
        var fnInfo = this._functions.get(item.fnName);
        var labelIndex = fnInfo.labels.get(item.name);
        if (labelIndex === undefined) {
            // We still can't find the label, so it can't exist.
            this._errors.push({line: item.line, col: label.col,
                    message: "Unknown label: \"" + item.name + "\"."});
        } else {
            // Update this label call.
            item.codes[item.index]   = (labelIndex & 0xFF000000) >> 24;
            item.codes[item.index+1] = (labelIndex & 0x00FF0000) >> 16;
            item.codes[item.index+2] = (labelIndex & 0x0000FF00) >> 8;
            item.codes[item.index+3] = (labelIndex & 0x000000FF);
        }
    }
};

/*
 * The CapturingErrorListener object's job is to receive errors from the Antlr
 * parsing, and store the information for later reference.
 */
function CapturingErrorListener(errors) {
    errorListener.call(this);
    this._errors = errors;
    return this;
};
CapturingErrorListener.prototype = Object.create(errorListener.prototype);
CapturingErrorListener.prototype.constructor = CapturingErrorListener;
/*
 * Receives the notification of a syntax error in the parsing/lexing of a 
 * program.
 */
CapturingErrorListener.prototype.syntaxError = function( recognizer, offendingSymbol, line, column, msg, e) {
    this._errors.push({line: line,
            col: column,
            message: msg,
            type: "error"});
}

/*
 * Helper function used to invoke Antlr on a Logo program using the LogoDefs and
 * LogoRefs listeners and return the results.
 */
function _lexAndParseLogoProgram(input) {
    // Parse the Logo program into a parse tree.
    var chars = new antlr4.InputStream(input);
    var lexer = new logoLexer.logoLexer(chars);
    var tokens = new antlr4.CommonTokenStream(lexer);
    var parser = new logoParser.logoParser(tokens);
    var exceptions = [];
    var errListener = new CapturingErrorListener(exceptions);
    lexer.removeErrorListeners();
    lexer.addErrorListener(errListener);
    parser.removeErrorListeners();
    parser.addErrorListener(errListener);
    parser.buildParseTrees = true;
    var tree = parser.program();

    // Run through the tree to gather scopes and check variables.
    var lDef = new LogoDefs(exceptions);
    antlr4.tree.ParseTreeWalker.DEFAULT.walk(lDef, tree);

    // Run through the tree to check procedure references.
    var globalScope = lDef.getGlobalScope();
    var scopes = lDef.getScopes();
    var lRef = new LogoRefs(scopes, exceptions);
    antlr4.tree.ParseTreeWalker.DEFAULT.walk(lRef, tree);

    // Return the results of the lexing/parsing operations.
    return {parseTree:   tree,
            globalScope: globalScope,
            scopes:      scopes,
            exceptions:  exceptions};
}

/*
 * Verifies that a program is syntactically valid, returning all errors found, 
 * if any.
 */
function verifyProgram(input) {
    // Run the Logo program through the Antlr lexer and parser.
    var results = _lexAndParseLogoProgram(input);

    // Return the results.
    return results.exceptions;
}

/*
 * Compiles a program to bytecode, returning the bytecode, or the syntax 
 * exceptions if any are found.
 */
function compileProgram() {
    // Run the Logo program through the Antlr lexer and parser.
    var input = editor.getValue();
    var results = _lexAndParseLogoProgram(input);

    if (results.exceptions.length === 0) {
        // Run through the tree to create the assembley code.
        var lAsm = new LogoAssembler(results.globalScope, results.scopes);
        antlr4.tree.ParseTreeWalker.DEFAULT.walk(lAsm, results.parseTree);
        var asmText = lAsm.getText();
		console.log(asmText);
		
        // Compile the assembley code to bytecode.
        var chars = new antlr4.InputStream(asmText);
        var lexer = new asmLexer.asmLexer(chars);
        var tokens = new antlr4.CommonTokenStream(lexer);
        var parser = new asmParser.asmParser(tokens);
        parser.buildParseTrees = true;
        var tree = parser.program();
        var comp = new AsmCompiler();
        antlr4.tree.ParseTreeWalker.DEFAULT.walk(comp, tree);

        // Return the resulting bytecode functions.
        var bytecode = comp.getBytecode();
        return {success: true, bytecode: bytecode};
    } else {
        // Exceptions were found, return them.
        return {success: false, exceptions: results.exceptions};
    }
}

logo = {};
logo.verifyProgram = verifyProgram;
logo.compileProgram = compileProgram;
