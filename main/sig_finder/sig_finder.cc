#include "absl/strings/ascii.h"
#include "absl/strings/match.h"
#include "ast/treemap/treemap.h"
#include "core/core.h"

#include "main/sig_finder/sig_finder.h"

using namespace std;

namespace sorbet::sig_finder {

ast::ExpressionPtr SigFinder::preTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
    auto loc = core::Loc(ctx.file, tree.loc());

    if (!this->narrowestClassDefRange.exists()) {
        // No narrowestClassDefRange yet, so take the loc of the first ClassDef we see
        // Usually this is the `<root>` class (whole file), but sometimes the caller might provide
        // us a specific ClassDef to look in if it has one (not necessarily root)
        this->narrowestClassDefRange = loc;
    } else if (loc.contains(this->queryLoc) && this->narrowestClassDefRange.contains(loc)) {
        // `loc` is contained in the current narrowestClassDefRange, and still contains `queryLoc`
        this->narrowestClassDefRange = loc;

        if (this->result_.has_value() && !loc.contains(ctx.locAt(this->result_->origSend->loc))) {
            // If there's a result and it's not contained in the new narrowest range, we have to toss it out
            // (Method defs and class defs are not necessarily sorted by their locs)
            this->result_ = nullopt;
        }
    }

    this->scopeContainsQueryLoc.emplace_back(loc.contains(this->queryLoc));

    return tree;
}

ast::ExpressionPtr SigFinder::postTransformClassDef(core::Context ctx, ast::ExpressionPtr tree) {
    ENFORCE(!this->scopeContainsQueryLoc.empty());
    this->scopeContainsQueryLoc.pop_back();

    return tree;
}

ast::ExpressionPtr SigFinder::preTransformSend(core::Context ctx, ast::ExpressionPtr tree) {
    auto &send = ast::cast_tree_nonnull<ast::Send>(tree);

    if (!resolver::TypeSyntax::isSig(ctx, send)) {
        return tree;
    }

    ENFORCE(!this->scopeContainsQueryLoc.empty());
    if (!this->scopeContainsQueryLoc.back()) {
        // Regardless of whether this send is after the queryLoc or inside the narrowestClassDefRange,
        // we're in a ClassDef whose scope doesn't contain the queryLoc.
        // (one case where this happens: nested Inner class)
        return tree;
    }

    auto currentLoc = ctx.locAt(tree.loc());
    if (!currentLoc.exists()) {
        // Defensive in case location information is disabled (e.g., certain fuzzer modes)
        return tree;
    }

    ENFORCE(this->narrowestClassDefRange.exists());

    if (!this->narrowestClassDefRange.contains(currentLoc)) {
        // This send occurs outside the current narrowest range we know of for a ClassDef that
        // still contains queryLoc, so even if this Send is after the queryLoc, it would not be
        // in the right scope.
        return tree;
    } else if (!(currentLoc.endPos() <= this->queryLoc.beginPos())) {
        // Query loc is not after the send
        return tree;
    }

    if (this->result_.has_value()) {
        // Method defs are not guaranteed to be sorted in order by their declLocs
        auto resultLoc = this->result_->origSend->loc;
        if (resultLoc.beginPos() < currentLoc.beginPos()) {
            // Found a method defined before the query but later than previous result: overwrite previous result
            auto parsedSig = resolver::TypeSyntax::parseSigTop(ctx, send, core::Symbols::untyped());
            this->result_ = make_optional<resolver::ParsedSig>(move(parsedSig));
            return tree;
        } else {
            // We've already found an earlier result, so the current is not the first
            return tree;
        }
    } else {
        // Haven't found a result yet, so this one is the best so far.
        auto parsedSig = resolver::TypeSyntax::parseSigTop(ctx, send, core::Symbols::untyped());
        this->result_ = make_optional<resolver::ParsedSig>(move(parsedSig));
        return tree;
    }
}

optional<resolver::ParsedSig> SigFinder::findSignature(core::Context ctx, ast::ExpressionPtr &tree,
                                                       core::Loc queryLoc) {
    SigFinder sigFinder(queryLoc);
    tree = ast::ShallowMap::apply(ctx, sigFinder, move(tree));
    return move(sigFinder.result_);
}

} // namespace sorbet::sig_finder
