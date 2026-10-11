# Changes to the pinned, private C++ sources. Each anchor must occur exactly
# once: a dependency update cannot silently drop or misapply parser behavior.
function(neverd_hermes_patch variable before after description)
  string(REPLACE "${before}" "" _without "${${variable}}")
  string(LENGTH "${${variable}}" _size)
  string(LENGTH "${_without}" _remaining)
  string(LENGTH "${before}" _change)
  math(EXPR _removed "${_size} - ${_remaining}")
  if(NOT _removed EQUAL _change)
    message(FATAL_ERROR "Pinned Hermes patch must match exactly once: ${description}")
  endif()
  string(REPLACE "${before}" "${after}" _result "${${variable}}")
  set(${variable} "${_result}" PARENT_SCOPE)
endfunction()

neverd_hermes_patch(_parser_patched
  [=[#include "JSParserImpl.h"]=]
  [=[#include "JSParserImpl.h"
#include "llvh/ADT/DenseSet.h"]=]
  "resource binding duplicate set")
neverd_hermes_patch(_parser_patched
  [=[        paramList.push_back(*new (context_)
                                ESTree::RestElementNode(spread->_argument));]=]
  [=[        paramList.push_back(*setLocation(
            spread, spread, new (context_) ESTree::RestElementNode(spread->_argument)));]=]
  "async-arrow rest locations")

neverd_hermes_patch(_parser_header
  [=[  /// Check whether the current token begins a Declaration.]=]
  [=[  bool checkResourceDeclaration(bool inFor = false);
  Optional<ESTree::VariableDeclarationNode *> parseResourceDeclaration(
      Param param, bool inFor = false);

  /// Check whether the current token begins a Declaration.]=]
  "resource grammar method declarations")
neverd_hermes_patch(_parser_patched
  [=[namespace detail {
]=]
  [=[namespace detail {

#include "HermesResourceDeclarations.inc"
]=]
  "resource grammar implementation")
neverd_hermes_patch(_parser_patched
  [=[Optional<ESTree::ProgramNode *> JSParserImpl::parseProgram() {
]=]
  [=[Optional<ESTree::ProgramNode *> JSParserImpl::parseProgram() {
  llvh::SaveAndRestore<bool> moduleAwait(
      paramAwait_, context_.getTransformCJSModules());
]=]
  "module await grammar parameter")
neverd_hermes_patch(_parser_patched
  [=[    ESTree::NodeList &stmtList) {
  if (checkDeclaration()) {]=]
  [=[    ESTree::NodeList &stmtList) {
  if (checkResourceDeclaration()) {
    auto decl = parseResourceDeclaration(ParamIn);
    if (!decl)
      return false;
    if (allowImportExport == AllowImportExport::Yes &&
        !context_.getTransformCJSModules() &&
        !context_.allowReturnOutsideFunction())
      error((*decl)->getSourceRange(), "resource declaration at script top level");
    stmtList.push_back(**decl);
  } else if (checkDeclaration()) {]=]
  "statement-list resource declarations")
neverd_hermes_patch(_parser_patched
  [=[  if (checkN(TokenKind::rw_var, TokenKind::rw_const, letIdent_)) {
    // Productions valid here:]=]
  [=[  if (checkResourceDeclaration(true)) {
    auto resource = parseResourceDeclaration(Param{}, true);
    if (!resource)
      return None;
    decl = *resource;
  } else if (checkN(TokenKind::rw_var, TokenKind::rw_const, letIdent_)) {
    // Productions valid here:]=]
  "for resource declarations")

# Newline state affects ASI and contextual keywords. Backtracking must restore
# it just as it already restores previous-token locations and stored tokens.
neverd_hermes_patch(_lexer_header
  [=[    /// Saved curCharPtr_ from the lexer.
]=]
  [=[    bool newLineBeforeCurrentToken_;

    /// Saved curCharPtr_ from the lexer.
]=]
  "lexer save-point newline member")
neverd_hermes_patch(_lexer_header
  [=[          loc_(lexer_->getCurLoc()),]=]
  [=[          newLineBeforeCurrentToken_(lexer_->isNewLineBeforeCurrentToken()),
          loc_(lexer_->getCurLoc()),]=]
  "lexer save-point newline capture")
neverd_hermes_patch(_lexer_header
  [=[      lexer_->prevTokenEndLoc_ = prevTokenEndLoc_;]=]
  [=[      lexer_->prevTokenEndLoc_ = prevTokenEndLoc_;
      lexer_->newLineBeforeCurrentToken_ = newLineBeforeCurrentToken_;]=]
  "lexer save-point newline restore")

# This is name validation, not lowering: retain the original declaration kind
# in the AST, and apply immutable lexical-name rules to resource declarations.
neverd_hermes_patch(_validator_patched
  [=[  else if (declaration->_kind == kw_.identConst)
    declKind = FunctionInfo::VarDecl::Kind::Const;]=]
  [=[  else if (declaration->_kind == kw_.identConst ||
           declaration->_kind->str() == "using" ||
           declaration->_kind->str() == "await using") {
    declKind = FunctionInfo::VarDecl::Kind::Const;
    if (declaration->_kind->str() == "await using" && forbidAwaitExpression_)
      sm_.error(declaration->getSourceRange(), "async disposal is not allowed here");
  }]=]
  "resource binding names and async context")
