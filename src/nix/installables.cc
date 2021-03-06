#include "command.hh"
#include "attr-path.hh"
#include "common-opts.hh"
#include "derivations.hh"
#include "eval-inline.hh"
#include "eval.hh"
#include "get-drvs.hh"
#include "store-api.hh"
#include "shared.hh"

#include <regex>

namespace nix {

Value * SourceExprCommand::getSourceExpr(EvalState & state)
{
    if (vSourceExpr) return vSourceExpr;

    auto sToplevel = state.symbols.create("_toplevel");

    vSourceExpr = state.allocValue();

    if (file != "") {
        Expr * e = state.parseExprFromFile(resolveExprPath(lookupFileArg(state, file)));
        state.eval(e, *vSourceExpr);
    }

    else {

        /* Construct the installation source from $NIX_PATH. */

        auto searchPath = state.getSearchPath();

        state.mkAttrs(*vSourceExpr, searchPath.size() + 1);

        mkBool(*state.allocAttr(*vSourceExpr, sToplevel), true);

        std::unordered_set<std::string> seen;

        for (auto & i : searchPath) {
            if (i.first == "") continue;
            if (seen.count(i.first)) continue;
            seen.insert(i.first);
#if 0
            auto res = state.resolveSearchPathElem(i);
            if (!res.first) continue;
            if (!pathExists(res.second)) continue;
            mkApp(*state.allocAttr(*vSourceExpr, state.symbols.create(i.first)),
                state.getBuiltin("import"),
                mkString(*state.allocValue(), res.second));
#endif
            Value * v1 = state.allocValue();
            mkPrimOpApp(*v1, state.getBuiltin("findFile"), state.getBuiltin("nixPath"));
            Value * v2 = state.allocValue();
            mkApp(*v2, *v1, mkString(*state.allocValue(), i.first));
            mkApp(*state.allocAttr(*vSourceExpr, state.symbols.create(i.first)),
                state.getBuiltin("import"), *v2);
        }

        vSourceExpr->attrs->sort();
    }

    return vSourceExpr;
}

ref<EvalState> SourceExprCommand::getEvalState()
{
    if (!evalState)
        evalState = std::make_shared<EvalState>(Strings{}, getStore());
    return ref<EvalState>(evalState);
}

Buildable Installable::toBuildable()
{
    auto buildables = toBuildables();
    if (buildables.size() != 1)
        throw Error("installable '%s' evaluates to %d derivations, where only one is expected", what(), buildables.size());
    return std::move(buildables[0]);
}

struct InstallableStoreDrv : Installable
{
    Path drvPath;

    InstallableStoreDrv(const Path & drvPath) : drvPath(drvPath) { }

    std::string what() override { return drvPath; }

    Buildables toBuildables() override
    {
        Buildable b = {drvPath};
        // FIXME: add outputs?
        return {b};
    }
};

struct InstallableStorePath : Installable
{
    Path storePath;

    InstallableStorePath(const Path & storePath) : storePath(storePath) { }

    std::string what() override { return storePath; }

    Buildables toBuildables() override
    {
        return {{"", {{"out", storePath}}}};
    }
};

struct InstallableValue : Installable
{
    SourceExprCommand & cmd;

    InstallableValue(SourceExprCommand & cmd) : cmd(cmd) { }

    Buildables toBuildables() override
    {
        auto state = cmd.getEvalState();

        auto v = toValue(*state);

        // FIXME
        std::map<string, string> autoArgs_;
        Bindings & autoArgs(*evalAutoArgs(*state, autoArgs_));

        DrvInfos drvs;
        getDerivations(*state, *v, "", autoArgs, drvs, false);

        Buildables res;

        PathSet drvPaths;

        for (auto & drv : drvs) {
            Buildable b{drv.queryDrvPath()};
            drvPaths.insert(b.drvPath);

            auto outputName = drv.queryOutputName();
            if (outputName == "")
                throw Error("derivation '%s' lacks an 'outputName' attribute", b.drvPath);

            b.outputs.emplace(outputName, drv.queryOutPath());

            res.push_back(std::move(b));
        }

        // Hack to recognize .all: if all drvs have the same drvPath,
        // merge the buildables.
        if (drvPaths.size() == 1) {
            Buildable b{*drvPaths.begin()};
            for (auto & b2 : res)
                b.outputs.insert(b2.outputs.begin(), b2.outputs.end());
            return {b};
        } else
            return res;
    }
};

struct InstallableExpr : InstallableValue
{
    std::string text;

    InstallableExpr(SourceExprCommand & cmd, const std::string & text)
         : InstallableValue(cmd), text(text) { }

    std::string what() override { return text; }

    Value * toValue(EvalState & state) override
    {
        auto v = state.allocValue();
        state.eval(state.parseExprFromString(text, absPath(".")), *v);
        return v;
    }
};

struct InstallableAttrPath : InstallableValue
{
    std::string attrPath;

    InstallableAttrPath(SourceExprCommand & cmd, const std::string & attrPath)
        : InstallableValue(cmd), attrPath(attrPath)
    { }

    std::string what() override { return attrPath; }

    Value * toValue(EvalState & state) override
    {
        auto source = cmd.getSourceExpr(state);

        // FIXME
        std::map<string, string> autoArgs_;
        Bindings & autoArgs(*evalAutoArgs(state, autoArgs_));

        Value * v = findAlongAttrPath(state, attrPath, autoArgs, *source);
        state.forceValue(*v);

        return v;
    }
};

// FIXME: extend
std::string attrRegex = R"([A-Za-z_][A-Za-z0-9-_+]*)";
static std::regex attrPathRegex(fmt(R"(%1%(\.%1%)*)", attrRegex));

static std::vector<std::shared_ptr<Installable>> parseInstallables(
    SourceExprCommand & cmd, ref<Store> store, Strings ss, bool useDefaultInstallables)
{
    std::vector<std::shared_ptr<Installable>> result;

    if (ss.empty() && useDefaultInstallables) {
        if (cmd.file == "")
            cmd.file = ".";
        ss = Strings{""};
    }

    for (auto & s : ss) {

        if (s.compare(0, 1, "(") == 0)
            result.push_back(std::make_shared<InstallableExpr>(cmd, s));

        else if (s.find("/") != std::string::npos) {

            auto path = store->toStorePath(store->followLinksToStore(s));

            if (store->isStorePath(path)) {
                if (isDerivation(path))
                    result.push_back(std::make_shared<InstallableStoreDrv>(path));
                else
                    result.push_back(std::make_shared<InstallableStorePath>(path));
            }
        }

        else if (s == "" || std::regex_match(s, attrPathRegex))
            result.push_back(std::make_shared<InstallableAttrPath>(cmd, s));

        else
            throw UsageError("don't know what to do with argument '%s'", s);
    }

    return result;
}

Buildables InstallablesCommand::toBuildables(ref<Store> store, RealiseMode mode)
{
    if (mode != Build)
        settings.readOnlyMode = true;

    Buildables buildables;

    PathSet pathsToBuild;

    for (auto & i : installables) {
        for (auto & b : i->toBuildables()) {
            if (b.drvPath != "")
                pathsToBuild.insert(b.drvPath);
            buildables.push_back(std::move(b));
        }
    }

    if (mode == DryRun)
        printMissing(store, pathsToBuild, lvlError);
    else if (mode == Build)
        store->buildPaths(pathsToBuild);

    return buildables;
}

PathSet InstallablesCommand::toStorePaths(ref<Store> store, RealiseMode mode)
{
    PathSet outPaths;

    for (auto & b : toBuildables(store, mode))
        for (auto & output : b.outputs)
            outPaths.insert(output.second);

    return outPaths;
}

void InstallablesCommand::prepare()
{
    installables = parseInstallables(*this, getStore(), _installables, useDefaultInstallables());
}

void InstallableCommand::prepare()
{
    auto installables = parseInstallables(*this, getStore(), {_installable}, false);
    assert(installables.size() == 1);
    installable = installables.front();
}

}
