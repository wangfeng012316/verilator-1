//*************************************************************************
// DESCRIPTION: Verilator: Removal of named begin blocks
//
// Code available from: http://www.veripool.org/verilator
//
// AUTHORS: Wilson Snyder with Paul Wasson, Duane Gabli
//
//*************************************************************************
//
// Copyright 2003-2009 by Wilson Snyder.  This program is free software; you can
// redistribute it and/or modify it under the terms of either the GNU
// Lesser General Public License Version 3 or the Perl Artistic License
// Version 2.0.
//
// Verilator is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
//*************************************************************************
// V3Begin's Transformations:
//
// Each module:
//	Look for BEGINs
//	    BEGIN(VAR...) -> VAR ... {renamed}
//	FOR -> WHILEs
//
// There are two scopes; named BEGINs change %m and variable scopes.
// Unnamed BEGINs change only variable, not $display("%m") scope.
//
//*************************************************************************

#include "config_build.h"
#include "verilatedos.h"
#include <cstdio>
#include <cstdarg>
#include <unistd.h>
#include <algorithm>
#include <vector>

#include "V3Global.h"
#include "V3Begin.h"
#include "V3Inst.h"
#include "V3Ast.h"

//######################################################################

class BeginVisitor : public AstNVisitor {
private:
    // STATE
    AstNodeModule*	m_modp;		// Current module
    AstNodeFTask* 	m_ftaskp;	// Current function/task
    string		m_namedScope;	// Name of begin blocks above us
    string		m_unnamedScope;	// Name of begin blocks, including unnamed blocks
    int			m_repeatNum;	// Repeat counter

    // METHODS
    static int debug() {
	static int level = -1;
	if (VL_UNLIKELY(level < 0)) level = v3Global.opt.debugSrcLevel(__FILE__);
	return level;
    }

    // VISITORS
    virtual void visit(AstNodeModule* nodep, AstNUser*) {
	m_modp = nodep;
	m_repeatNum = 0;
	nodep->iterateChildren(*this);
	m_modp = NULL;
    }
    virtual void visit(AstNodeFTask* nodep, AstNUser*) {
	m_ftaskp = nodep;
	nodep->iterateChildren(*this);
	m_ftaskp = NULL;
    }
    virtual void visit(AstBegin* nodep, AstNUser*) {
	// Begin blocks were only useful in variable creation, change names and delete
	UINFO(8,"  "<<nodep<<endl);
	string oldScope = m_namedScope;
	string oldUnnamed = m_unnamedScope;
	{
	    //UINFO(8,"nname "<<m_namedScope<<endl);
	    if (nodep->name() != "") {  // Else unneeded unnamed block
		// Create data for dotted variable resolution
		string dottedname = nodep->name() + "__DOT__";  // So always found
		string::size_type pos;
		while ((pos=dottedname.find("__DOT__")) != string::npos) {
		    string ident = dottedname.substr(0,pos);
		    dottedname = dottedname.substr(pos+strlen("__DOT__"));
		    if (!nodep->unnamed()) {
			if (m_namedScope=="") m_namedScope = ident;
			else m_namedScope = m_namedScope + "__DOT__"+ident;
		    }
		    if (m_unnamedScope=="") m_unnamedScope = ident;
		    else m_unnamedScope = m_unnamedScope + "__DOT__"+ident;
		    // Create CellInline for dotted var resolution
		    AstCellInline* inlinep = new AstCellInline(nodep->fileline(),
							       m_unnamedScope, "__BEGIN__");
		    m_modp->addInlinesp(inlinep);  // Must be parsed before any AstCells
		}
	    }

	    // Remap var names and replace lower Begins
	    nodep->iterateChildren(*this);

	    if (AstNode* stmtsp = nodep->stmtsp()) {
		stmtsp->unlinkFrBackWithNext();
		nodep->replaceWith(stmtsp);
	    } else {
		nodep->unlinkFrBack();
	    }
	    pushDeletep(nodep); nodep=NULL;
	}
	m_namedScope = oldScope;
	m_unnamedScope = oldUnnamed;
    }
    virtual void visit(AstVar* nodep, AstNUser*) {
	if (m_unnamedScope != "") {
	    // Rename it
	    nodep->name(m_unnamedScope+"__DOT__"+nodep->name());
	    // Move to module
	    nodep->unlinkFrBack();
	    if (m_ftaskp) m_ftaskp->addStmtsp(nodep);   // Begins under funcs just move into the func
	    else m_modp->addStmtp(nodep);
	}
    }
    virtual void visit(AstCell* nodep, AstNUser*) {
	UINFO(8,"   CELL "<<nodep<<endl);
	if (m_namedScope != "") {
	    // Rename it
	    nodep->name(m_namedScope+"__DOT__"+nodep->name());
	    UINFO(8,"     rename to "<<nodep->name()<<endl);
	    // Move to module
	    nodep->unlinkFrBack();
	    m_modp->addStmtp(nodep);
	}
    }
    virtual void visit(AstFor* nodep, AstNUser*) {
	// So later optimizations don't need to deal with them,
	//    FOR(init,cond,assign,body) -> init,WHILE(cond) { body, assign }
	AstNode* initsp = nodep->initsp(); if (initsp) initsp->unlinkFrBackWithNext();
	AstNode* condp = nodep->condp(); if (condp) condp->unlinkFrBackWithNext();
	AstNode* incsp = nodep->incsp(); if (incsp) incsp->unlinkFrBackWithNext();
	AstNode* bodysp = nodep->bodysp(); if (bodysp) bodysp->unlinkFrBackWithNext();
	bodysp = bodysp->addNext(incsp);
	AstNode* newp = new AstWhile(nodep->fileline(),
				     condp,
				     bodysp);
	initsp = initsp->addNext(newp);
	newp = initsp;
	nodep->replaceWith(newp);
	nodep->deleteTree(); nodep=NULL;
    }
    virtual void visit(AstRepeat* nodep, AstNUser*) {
	// So later optimizations don't need to deal with them,
	//    REPEAT(count,body) -> loop=count,WHILE(loop>0) { body, loop-- }
	// Note var can be signed or unsigned based on original number.
	AstNode* countp = nodep->countp()->unlinkFrBackWithNext();
   	string name = string("__Vrepeat")+cvtToStr(m_repeatNum++);
	AstVar* varp = new AstVar(nodep->fileline(), AstVarType::BLOCKTEMP, name, AstLogicPacked(), countp->width());
	m_modp->addStmtp(varp);
	AstNode* initsp = new AstAssign(nodep->fileline(), new AstVarRef(nodep->fileline(), varp, true),
					countp);
	AstNode* decp = new AstAssign(nodep->fileline(), new AstVarRef(nodep->fileline(), varp, true),
				      new AstSub(nodep->fileline(), new AstVarRef(nodep->fileline(), varp, false),
						 new AstConst(nodep->fileline(), 1)));
	AstNode* condp;
	if (countp->isSigned()) {
	    condp = new AstGtS(nodep->fileline(), new AstVarRef(nodep->fileline(), varp, false),
			       new AstConst(nodep->fileline(), 0));
	} else {
	    condp = new AstGt (nodep->fileline(), new AstVarRef(nodep->fileline(), varp, false),
			       new AstConst(nodep->fileline(), 0));
	}
	AstNode* bodysp = nodep->bodysp(); if (bodysp) bodysp->unlinkFrBackWithNext();
	bodysp = bodysp->addNext(decp);
	AstNode* newp = new AstWhile(nodep->fileline(),
				     condp,
				     bodysp);
	initsp = initsp->addNext(newp);
	newp = initsp;
	nodep->replaceWith(newp);
	nodep->deleteTree(); nodep=NULL;
    }
    virtual void visit(AstScopeName* nodep, AstNUser*) {
	// If there's a %m in the display text, we add a special node that will contain the name()
	// Similar code in V3Inline
	if (m_namedScope != "") {
	    // To keep correct visual order, must add before other Text's
	    AstNode* afterp = nodep->scopeAttrp();
	    if (afterp) afterp->unlinkFrBackWithNext();
	    nodep->scopeAttrp(new AstText(nodep->fileline(), (string)"__DOT__"+m_namedScope));
	    if (afterp) nodep->scopeAttrp(afterp);
	}
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstCoverDecl* nodep, AstNUser*) {
	// Don't need to fix path in coverage statements, they're not under
	// any BEGINs, but V3Coverage adds them all under the module itself.
	nodep->iterateChildren(*this);
    }
    virtual void visit(AstNode* nodep, AstNUser*) {
	nodep->iterateChildren(*this);
    }
public:
    // CONSTUCTORS
    BeginVisitor(AstNetlist* nodep) {
	m_modp = NULL;
	m_ftaskp = NULL;
	m_repeatNum = 0;
	nodep->accept(*this);
    }
    virtual ~BeginVisitor() {}
};

//######################################################################
// Task class functions

void V3Begin::debeginAll(AstNetlist* nodep) {
    UINFO(2,__FUNCTION__<<": "<<endl);
    BeginVisitor bvisitor (nodep);
}
