#pragma once

#include "last_getopt_opts.h"

#include <util/generic/map.h>
#include <util/generic/string.h>
#include <util/generic/vector.h>

#include <functional>

//! Mode function with vector of cli arguments.
using TMainFunctionPtrV = std::function<int(const TVector<TString>&)> ;
using TMainFunctionRawPtrV = int (*)(const TVector<TString>& argv);

//! Mode function with classic argc and argv arguments.
using TMainFunctionPtr = std::function<int(int, const char**)> ;
using TMainFunctionRawPtr = int (*)(const int argc, const char** argv);

//! Mode class with vector of cli arguments.
class TMainClassV {
public:
    virtual int operator()(const TVector<TString>& argv) = 0;
    virtual ~TMainClassV() = default;
};

//! Mode class with classic argc and argv arguments.
class TMainClass {
public:
    virtual int operator()(int argc, const char** argv) = 0;
    virtual ~TMainClass() = default;
};

//! Function to handle '--version' parameter 
typedef void (*TVersionHandlerPtr)(); 
 
/*! Main class for handling different modes in single tool.
 *
 * You can add modes for this class, use autogenerated help with
 * list of modes and automaticly call necessary mode in run().
 *
 * In first argv element mode get joined by space tool name and
 * current mode name.
 */
class TModChooser {
public:
    TModChooser(); 
    ~TModChooser();

public:
    void AddMode(const TString& mode, TMainFunctionRawPtr func, const TString& description, bool hidden = false, bool noCompletion = false);
    void AddMode(const TString& mode, TMainFunctionRawPtrV func, const TString& description, bool hidden = false, bool noCompletion = false);
    void AddMode(const TString& mode, TMainFunctionPtr func, const TString& description, bool hidden = false, bool noCompletion = false);
    void AddMode(const TString& mode, TMainFunctionPtrV func, const TString& description, bool hidden = false, bool noCompletion = false);
    void AddMode(const TString& mode, TMainClass* func, const TString& description, bool hidden = false, bool noCompletion = false);
    void AddMode(const TString& mode, TMainClassV* func, const TString& description, bool hidden = false, bool noCompletion = false);

    //! Hidden groups won't be displayed in 'help' block
    void AddGroupModeDescription(const TString& description, bool hidden = false, bool noCompletion = false);

    //! Set default mode (if not specified explicitly)
    void SetDefaultMode(const TString& mode);

    void AddAlias(const TString& alias, const TString& mode);

    //! Set main program description.
    void SetDescription(const TString& descr);

    //! Set modes help option name (-? is by default)
    void SetModesHelpOption(const TString& helpOption);
 
    //! Specify handler for '--version' parameter 
    void SetVersionHandler(TVersionHandlerPtr handler); 
 
    //! Set description show mode
    void SetSeparatedMode(bool separated = true);

    //! Set separation string
    void SetSeparationString(const TString& str);

    //! Set short command representation in Usage block
    void SetPrintShortCommandInUsage(bool printShortCommandInUsage);

    void DisableSvnRevisionOption();

    void AddCompletions(TString progName, const TString& name = "completion", bool hidden = false, bool noCompletion = false);

    /*! Run appropriate mode.
     *
     * In this method following things happen:
     *   1) If first argument is -h/--help/-? then print short description of
     *      all modes and exit with zero code.
     *   2) If first argument is -v/--version and version handler is specified, 
     *      then call it and exit with zero code.
     *   3) Find mode with the same name as first argument. If it's found then 
     *      call it and return its return code.
     *   4) If appropriate mode is not found - return non-zero code.
     */
    int Run(int argc, const char** argv) const;

    //! Run appropriate mode. Same as Run(const int, const char**)
    int Run(const TVector<TString>& argv) const;

    void PrintHelp(const TString& progName) const;

    struct TMode {
        TString Name;
        TMainClass* Main;
        TString Description;
        bool Hidden;
        bool NoCompletion;
        TVector<TString> Aliases;

        TMode()
            : Main(nullptr)
        {
        }

        TMode(const TString& name, TMainClass* main, const TString& descr, bool hidden, bool noCompletion);

        // Full name includes primary name and aliases. Also, will add ANSI colors.
        size_t CalculateFullNameLen() const;
        TString FormatFullName(size_t pad) const;
    };

    TVector<const TMode*> GetUnsortedModes() const {
        auto ret = TVector<const TMode*>(Reserve(UnsortedModes.size()));
        for (auto& mode : UnsortedModes) {
            ret.push_back(mode.Get());
        }
        return ret;
    }

    TVersionHandlerPtr GetVersionHandler() const;

    bool IsSvnRevisionOptionDisabled() const;

private:
    //! Main program description.
    TString Description;

    //! Help option for modes. 
    TString ModesHelpOption;
 
    //! Wrappers around all modes.
    TVector<THolder<TMainClass>> Wrappers;

    //! Modes
    TMap<TString, TMode*> Modes;
 
    TString DefaultMode;

    //! Handler for '--version' parameter 
    TVersionHandlerPtr VersionHandler; 

    //! When set to true, show descriptions unsorted and display separators
    bool ShowSeparated;

    //! When set to true, disables --svnrevision option, useful for opensource (git hosted) projects
    bool SvnRevisionOptionDisabled;

    //! When true - will print only 'mode name' in 'Usage' block
    bool PrintShortCommandInUsage;

    //! Text string used when displaying each separator
    TString SeparationString;

    //! Unsorted list of options
    TVector<THolder<TMode>> UnsortedModes;

    //! Mode that generates completions
    THolder<TMainClass> CompletionsGenerator;
};

//! Mode class that allows introspecting its console arguments.
class TMainClassArgs: public TMainClass {
public:
    int operator()(int argc, const char** argv) final;

public:
    //! Run this mode.
    int Run(int argc, const char** argv);

    //! Get console arguments for this mode.
    const NLastGetopt::TOpts& GetOptions();

protected:
    //! Fill given empty `TOpts` with options.
    virtual void RegisterOptions(NLastGetopt::TOpts& opts);

    //! Actual mode logic. Takes parsed options and returns exit code.
    virtual int DoRun(NLastGetopt::TOptsParseResult&& parsedOptions) = 0;

private:
    TMaybe<NLastGetopt::TOpts> Opts_;
};

//! Mode class that uses sub-modes to dispatch commands further.
class TMainClassModes: public TMainClass {
public:
    int operator()(int argc, const char** argv) final;

public:
    //! Run this mode.
    int Run(int argc, const char** argv);

    //! Get sub-modes for this mode.
    const TModChooser& GetSubModes();

protected:
    //! Fill given modchooser with sub-modes.
    virtual void RegisterModes(TModChooser& modes);

private:
    TMaybe<TModChooser> Modes_;
};
