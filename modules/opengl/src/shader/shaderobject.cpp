/*********************************************************************************
 *
 * Inviwo - Interactive Visualization Workshop
 *
 * Copyright (c) 2012-2020 Inviwo Foundation
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 * list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 *********************************************************************************/

#include <modules/opengl/shader/shaderobject.h>

#include <inviwo/core/io/textfilereader.h>
#include <inviwo/core/util/filesystem.h>
#include <inviwo/core/util/stdextensions.h>
#include <inviwo/core/util/zip.h>
#include <inviwo/core/util/ostreamjoiner.h>
#include <modules/opengl/openglexception.h>
#include <modules/opengl/shader/shadermanager.h>
#include <modules/opengl/shader/shaderutils.h>
#include <modules/opengl/openglcapabilities.h>

#include <fmt/format.h>

#include <cstdio>
#include <fstream>
#include <string>
#include <cctype>

#include <sml/sml.hpp>
namespace sml = boost::sml;

namespace inviwo {

namespace psm {

std::string indent(const std::string& block, size_t indent) {
    std::stringstream ss;
    bool first = true;
    for (auto&& line : splitString(block, '\n')) {
        if (!first) {
            ss << '\n' << std::string(indent, ' ');
        }
        first = false;
        ss << line;
    }

    return ss.str();
}

// States
struct EmptyLine {};
struct LineCom {};
struct BlockCom1 {};
struct BlockCom2 {};
struct Include {};
struct Replace {};
struct Code {};
struct Slash {};
struct Error {};

// Events
struct Char {
    using It = typename std::string::const_iterator;

    It curr;
    It end;

    bool peek(std::string_view match) {
        return std::mismatch(curr, end, match.begin(), match.end()).second == match.end();
    };
};
struct Eof {};

// States
struct State {
    std::ostream& output;
    LineNumberResolver& lnr;
    const std::string& key;
    std::unordered_map<typename ShaderSegment::Type, std::vector<ShaderSegment>> replacements;
    std::function<std::optional<std::pair<std::string, std::string>>(const std::string&)> getSource;
    size_t lines = 0;
    size_t column = 0;
};

struct Errors {
    std::exception_ptr exception;
};

struct Psm {
    auto operator()() const noexcept {
        using namespace sml;

        // actions
        auto print = [](Char c, State& s) {
            s.output << *c.curr;
            ++s.column;
        };
        auto nl = [](State& s) {
            s.lnr.addLine(s.key, ++s.lines);
            s.column = 0;
        };
        auto il = [](State& s) { ++s.lines; };
        auto include = [](Char c, State& s) {
            auto pathBegin = std::find(c.curr, c.end, '"');
            if (pathBegin == c.end) {
                throw OpenGLException{
                    fmt::format("Invalid include found at {}({})", s.key, s.lines + 1),
                    SourceContext(s.key, s.key, "", static_cast<int>(s.lines + 1))};
            }

            auto pathEnd = std::find(pathBegin + 1, c.end, '"');
            if (pathEnd == c.end) {
                throw OpenGLException{
                    fmt::format("Invalid include found at {}({})", s.key, s.lines + 1),
                    SourceContext(s.key, s.key, "", static_cast<int>(s.lines + 1))};
            }

            auto path = std::string{pathBegin + 1, pathEnd};

            try {
                if (auto res = s.getSource(path)) {
                    utilgl::parseShaderSource(res->first, res->second, s.output, s.lnr,
                                              s.replacements, s.getSource);
                    ++s.lines;
                } else {
                    s.lnr.addLine(s.key, ++s.lines);
                }
            } catch (const OpenGLException& e) {
                throw OpenGLException(
                    e.getMessage(), SourceContext(s.key, s.key, "", static_cast<int>(s.lines + 1)));
            }
        };

        auto replace = [](Char c, State& s) {
            using SegType = typename ShaderSegment::Type;
            const auto lineEnd = std::find(c.curr, c.end, '\n');
            const auto type = SegType{trim(std::string{c.curr, lineEnd})};
            const auto it = s.replacements.find(type);
            if (it != s.replacements.end()) {
                for (auto segIt = it->second.begin(); segIt != it->second.end(); ++segIt) {
                    const auto& segment = *segIt;
                    const auto snippet = psm::indent(segment.snippet, s.column);
                    utilgl::parseShaderSource(
                        fmt::format("{}[{},{}]", segment.name, segment.type.getString(),
                                    segment.priority),
                        snippet, s.output, s.lnr, s.replacements, s.getSource);
                    if (std::next(segIt) != it->second.end()) {
                        s.output << '\n' << std::string(s.column, ' ');
                    }
                }
                ++s.lines;
            }
        };

        auto onError = [](Errors& err) { err.exception = std::current_exception(); };

        // guards
        auto blank = [](Char c) { return std::isblank(*c.curr); };
        auto eol = [](Char c) { return *c.curr == '\n'; };
        auto slash = [](Char c) { return *c.curr == '/'; };
        auto star = [](Char c) { return *c.curr == '*'; };
        auto inc = [](Char c) { return c.peek("#include"); };

        auto repl = [](Char c, State& s) {
            for (auto& [key, val] : s.replacements) {
                if (c.peek(key.getString())) return true;
            }
            return false;
        };

        // clang-format off
        return make_transition_table(
           *state<EmptyLine> + event<Char> [blank]    / (print)     = state<EmptyLine>,
            state<EmptyLine> + event<Char> [eol]      / (print, nl) = state<EmptyLine>,
            state<EmptyLine> + event<Char> [slash]    / (print)     = state<Slash>,
            state<EmptyLine> + event<Char> [inc]      / (include)   = state<Include>,
            state<EmptyLine> + event<Char> [repl]     / (replace)   = state<Replace>,
            state<EmptyLine> + event<Char>            / (print)     = state<Code>,
            state<EmptyLine> + event<Eof>             / (nl)        = X,

            state<Slash>     + event<Char> [slash]    / (print)     = state<LineCom>,
            state<Slash>     + event<Char> [star]     / (print)     = state<BlockCom1>,
            state<Slash>     + event<Char> [eol]      / (print, nl) = state<EmptyLine>,
            state<Slash>     + event<Char>            / (print)     = state<Code>,
            state<Slash>     + event<Eof>             / (nl)        = X,

            state<LineCom>   + event<Char> [eol]      / (print, nl) = state<EmptyLine>,
            state<LineCom>   + event<Char>            / (print)     = state<LineCom>,
            state<LineCom>   + event<Eof>             / (nl)        = X,

            state<BlockCom1> + event<Char> [star]     / (print)     = state<BlockCom2>,
            state<BlockCom1> + event<Char> [eol]      / (print, nl) = state<BlockCom1>,
            state<BlockCom1> + event<Char>            / (print)     = state<BlockCom1>,
            state<BlockCom2> + event<Char> [slash]    / (print)     = state<Code>,
            state<BlockCom2> + event<Char> [star]     / (print)     = state<BlockCom2>,
            state<BlockCom2> + event<Char> [eol]      / (print, nl) = state<BlockCom1>,
            state<BlockCom2> + event<Char>            / (print)     = state<BlockCom1>,

            state<Include>   + event<Char> [eol]      / (print)     = state<EmptyLine>,
            state<Include>   + event<Char>                          = state<Include>,
            state<Include>   + event<Eof>             / (il)        = X,

            state<Replace>   + event<Char> [eol]      / (print)     = state<EmptyLine>,
            state<Replace>   + event<Char>                          = state<Replace>,
            state<Replace>   + event<Eof>             / (nl)        = X,

            state<Code>      + event<Char> [slash]    / (print)     = state<Slash>,
            state<Code>      + event<Char> [eol]      / (print, nl) = state<EmptyLine>,
            state<Code>      + event<Char>            / (print)     = state<Code>,
            state<Code>      + event<Eof>             / (nl)        = X,


           *state<Error>     + exception<_>           / (onError)   = X

        );
        // clang-format on
    }
};
}  // namespace psm

void utilgl::parseShaderSource(
    const std::string& key, const std::string& source, std::ostream& output,
    LineNumberResolver& lnr,
    std::unordered_map<typename ShaderSegment::Type, std::vector<ShaderSegment>> replacements,
    std::function<std::optional<std::pair<std::string, std::string>>(const std::string&)>
        getSource) {

    size_t lines = 0;
    psm::State state{output, lnr, key, replacements, getSource, lines};
    psm::Errors errors;

    sml::sm<psm::Psm> sm{state, errors};
    for (auto it = source.begin(); it != source.end(); ++it) {
        sm.process_event(psm::Char{it, source.end()});
    }
    sm.process_event(psm::Eof{});
    if (errors.exception) {
        std::rethrow_exception(errors.exception);
    } else if (!sm.is(sml::X)) {
        throw OpenGLException{fmt::format("Parsing of '{}' ended prematurely", key),
                              IVW_CONTEXT_CUSTOM("ParseShaderSource")};
    }
}

std::string ShaderObject::InDeclaration::toString() const {
    return fmt::format(decl, fmt::arg("type", type), fmt::arg("name", name),
                       fmt::arg("location", location));
}

std::string ShaderObject::OutDeclaration::toString() const {
    return fmt::format(decl, fmt::arg("type", type), fmt::arg("name", name),
                       fmt::arg("location", location));
}

ShaderObject::ShaderObject(ShaderType shaderType, std::shared_ptr<const ShaderResource> resource)
    : shaderType_{shaderType}, id_{glCreateShader(shaderType)}, resource_{resource} {
    if (!shaderType_) {
        glDeleteShader(id_);
        throw OpenGLException("Invalid shader type", IVW_CONTEXT);
    }

    // Help developer to spot errors
    std::string fileExtension = filesystem::getFileExtension(resource_->key());
    if (fileExtension != shaderType_.extension()) {
        LogWarn("File extension does not match shader type: "
                << resource_->key() << "\n    expected extension: " << shaderType_.extension());
    }

    if (shaderType_ == ShaderType::Fragment) {
        addStandardFragmentOutDeclarations();
    } else if (shaderType_ == ShaderType::Vertex) {
        addStandardVertexInDeclarations();
    }
}

ShaderObject::ShaderObject(std::shared_ptr<const ShaderResource> resource)
    : ShaderObject(ShaderType::get(filesystem::getFileExtension(resource->key())), resource) {}

ShaderObject::ShaderObject(ShaderType shaderType, std::string fileName)
    : ShaderObject(shaderType, loadResource(fileName)) {}

ShaderObject::ShaderObject(std::string fileName)
    : ShaderObject(ShaderType::get(filesystem::getFileExtension(fileName)),
                   loadResource(fileName)) {}

ShaderObject::ShaderObject(GLenum shaderType, std::string fileName)
    : ShaderObject(ShaderType(shaderType), loadResource(fileName)) {}

ShaderObject::ShaderObject(const ShaderObject& rhs)
    : shaderType_(rhs.shaderType_)
    , id_(glCreateShader(rhs.shaderType_))
    , resource_(rhs.resource_)
    , inDeclarations_(rhs.inDeclarations_)
    , outDeclarations_(rhs.outDeclarations_)
    , shaderDefines_(rhs.shaderDefines_)
    , shaderExtensions_(rhs.shaderExtensions_)
    , sourceProcessed_{}
    , includeResources_{}
    , lnr_{}
    , callbacks_{}
    , resourceCallbacks_{} {}

ShaderObject::ShaderObject(ShaderObject&& rhs) noexcept
    : shaderType_(rhs.shaderType_)
    , id_(rhs.id_)
    , resource_(std::move(rhs.resource_))
    , inDeclarations_(std::move(rhs.inDeclarations_))
    , outDeclarations_(std::move(rhs.outDeclarations_))
    , shaderDefines_(std::move(rhs.shaderDefines_))
    , shaderExtensions_(std::move(rhs.shaderExtensions_))
    , sourceProcessed_{}
    , includeResources_{}
    , lnr_{}
    , callbacks_{}
    , resourceCallbacks_{} {

    rhs.id_ = 0;
    rhs.resourceCallbacks_.clear();
}

ShaderObject& ShaderObject::operator=(const ShaderObject& that) {
    ShaderObject copy(that);
    std::swap(shaderType_, copy.shaderType_);
    std::swap(id_, copy.id_);
    std::swap(resource_, copy.resource_);
    std::swap(inDeclarations_, copy.inDeclarations_);
    std::swap(outDeclarations_, copy.outDeclarations_);
    std::swap(shaderDefines_, copy.shaderDefines_);
    std::swap(shaderExtensions_, copy.shaderExtensions_);
    std::swap(sourceProcessed_, copy.sourceProcessed_);
    std::swap(includeResources_, copy.includeResources_);
    std::swap(lnr_, copy.lnr_);
    std::swap(callbacks_, copy.callbacks_);
    std::swap(resourceCallbacks_, copy.resourceCallbacks_);
    return *this;
}

ShaderObject& ShaderObject::operator=(ShaderObject&& that) noexcept {
    ShaderObject copy(std::move(that));
    std::swap(shaderType_, copy.shaderType_);
    std::swap(id_, copy.id_);
    std::swap(resource_, copy.resource_);
    std::swap(inDeclarations_, copy.inDeclarations_);
    std::swap(outDeclarations_, copy.outDeclarations_);
    std::swap(shaderDefines_, copy.shaderDefines_);
    std::swap(shaderExtensions_, copy.shaderExtensions_);
    std::swap(sourceProcessed_, copy.sourceProcessed_);
    std::swap(includeResources_, copy.includeResources_);
    std::swap(lnr_, copy.lnr_);
    std::swap(callbacks_, copy.callbacks_);
    std::swap(resourceCallbacks_, copy.resourceCallbacks_);
    return *this;
}

ShaderObject::~ShaderObject() { glDeleteShader(id_); }

GLuint ShaderObject::getID() const { return id_; }

std::string ShaderObject::getFileName() const { return resource_->key(); }

std::shared_ptr<const ShaderResource> ShaderObject::getResource() const { return resource_; }

const std::vector<std::shared_ptr<const ShaderResource>>& ShaderObject::getResources() const {
    return includeResources_;
}

ShaderType ShaderObject::getShaderType() const { return shaderType_; }

std::shared_ptr<const ShaderResource> ShaderObject::loadResource(std::string fileName) {
    return utilgl::findShaderResource(fileName);
}

void ShaderObject::build() {
    preprocess();
    upload();
    compile();
}

void ShaderObject::preprocess() {
    resourceCallbacks_.clear();
    lnr_.clear();
    auto holdOntoResources = includeResources_;  // Don't release until we have processed again.
    includeResources_.clear();

    std::ostringstream source;
    addDefines(source);
    parseSource(source);
    sourceProcessed_ = source.str();
}

void ShaderObject::addDefines(std::ostringstream& source) {
    auto capa = ShaderManager::getPtr()->getOpenGLCapabilities();
    const auto current = capa->getCurrentShaderVersion();

    source << "#version " << current.getVersionAndProfileAsString() << "\n";
    lnr_.addLine("Version", 0);

    for (const auto& se : shaderExtensions_) {
        source << "#extension " << se.first << " : " << (se.second ? "enable" : "disable") << "\n";
        lnr_.addLine("Extensions", 0);
    }

    if (current.hasProfile()) {
        source << "#define GLSL_PROFILE_" + toUpper(current.getProfile()) + "\n";
        lnr_.addLine("Header", 0);
    }

    int lastVersion = -1;
    for (size_t i = capa->getCurrentShaderIndex(); i < capa->getNumberOfShaderVersions(); i++) {
        const auto version = capa->getShaderVersion(i);
        if (lastVersion != version.getVersion()) {
            source << "#define GLSL_VERSION_" << version.getVersionAsString() << "\n";
            lnr_.addLine("Header", 0);
            lastVersion = version.getVersion();
        }
    }

    if (capa->getMaxProgramLoopCount() > 0) {
        source << "#define MAX_PROGRAM_LOOP_COUNT " << capa->getMaxProgramLoopCount() << "\n";
        lnr_.addLine("Header", 0);
    }

    size_t defineLines = 0;
    for (const auto& sd : shaderDefines_) {
        auto lines = std::count(sd.second.begin(), sd.second.end(), '\n');

        if (sd.second.empty() || (sd.first.size() + sd.second.size() < 60 && lines == 0)) {
            source << "#define " << sd.first << " " << sd.second << "\n";
            ++lines;
        } else {
            source << "#define " << sd.first << " \\\n    " << sd.second << "\n";
            lines += 2;
        }
        for (int i = 0; i != lines; ++i) {
            lnr_.addLine("Defines", ++defineLines);
        }
    }

    for (auto decl : outDeclarations_) {
        source << decl.toString() << "\n";
        lnr_.addLine("Out Declaration", 0);
    }
    for (auto decl : inDeclarations_) {
        source << decl.toString() << "\n";
        lnr_.addLine("In Declaration", 0);
    }
}

void ShaderObject::addStandardFragmentOutDeclarations() {
    addOutDeclaration("FragData0", 0);
    addOutDeclaration("PickingData", 1);
}

void ShaderObject::addStandardVertexInDeclarations() {
    addInDeclaration(InDeclaration{"in_Vertex", 0});
    addInDeclaration(InDeclaration{"in_Normal", 1, "vec3"});
    addInDeclaration(InDeclaration{"in_Color", 2});
    addInDeclaration(InDeclaration{"in_TexCoord", 3, "vec3"});
}

void ShaderObject::parseSource(std::ostringstream& output) {
    includeResources_.push_back(resource_);
    resourceCallbacks_.push_back(
        resource_->onChange([this](const ShaderResource*) { callbacks_.invoke(this); }));

    auto getSource =
        [this](const std::string& path) -> std::optional<std::pair<std::string, std::string>> {
        auto inc = ShaderManager::getPtr()->getShaderResource(path);
        if (!inc) {
            throw OpenGLException(
                fmt::format("Include file '{}' not found in shader search paths.", path),
                IVW_CONTEXT);
        }
        // Only include files once.
        if (util::find(includeResources_, inc) == includeResources_.end()) {
            includeResources_.push_back(inc);
            resourceCallbacks_.push_back(
                inc->onChange([this](const ShaderResource*) { callbacks_.invoke(this); }));
            return std::pair{inc->key(), inc->source()};
        } else {
            return std::nullopt;
        }
    };

    std::sort(shaderSegments_.begin(), shaderSegments_.end(),
              [](const ShaderSegment& a, const ShaderSegment& b) {
                  return std::tie(a.type, a.priority) < std::tie(b.type, b.priority);
              });
    std::unordered_map<typename ShaderSegment::Type, std::vector<ShaderSegment>> replacements;
    for (const auto& segment : shaderSegments_) {
        replacements[segment.type].push_back(segment);
    }

    utilgl::parseShaderSource(resource_->key(), resource_->source(), output, lnr_, replacements,
                              getSource);
}

std::string ShaderObject::resolveLog(const std::string& compileLog) const {
    std::ostringstream result;
    std::istringstream origShaderInfoLog(compileLog);

    std::string curLine;
    while (std::getline(origShaderInfoLog, curLine)) {
        if (!curLine.empty()) {
            const int origLineNumber = utilgl::getLogLineNumber(curLine);
            if (origLineNumber > 0) {
                const auto res = lnr_.resolveLine(origLineNumber);
                result << "\n"
                       << res.first << " (" << res.second
                       << "): " << curLine.substr(curLine.find(":") + 1);
            } else {
                result << "\n" << curLine;
            }
        }
    }

    return std::move(result).str();
}

void ShaderObject::upload() {
    const char* source = sourceProcessed_.c_str();
    glShaderSource(id_, 1, &source, nullptr);
    LGL_ERROR;
}

bool ShaderObject::isReady() const {
    GLint res = GL_FALSE;
    glGetShaderiv(id_, GL_COMPILE_STATUS, &res);
    return res == GL_TRUE;
}

void ShaderObject::compile() {
    glCompileShader(id_);
    if (!isReady()) {
        throw OpenGLException(resource_->key() + " " + resolveLog(utilgl::getShaderInfoLog(id_)),
                              IVW_CONTEXT);
    }

#ifdef IVW_DEBUG
    const auto log = utilgl::getShaderInfoLog(id_);
    if (!log.empty()) {
        util::log(IVW_CONTEXT, resource_->key() + " " + resolveLog(log), LogLevel::Info,
                  LogAudience::User);
    }
#endif
}

void ShaderObject::addShaderDefine(const std::string& name, const std::string& value) {
    shaderDefines_[name] = value;
}
void ShaderObject::setShaderDefine(const std::string& name, bool exists, const std::string& value) {
    if (exists) {
        addShaderDefine(name, value);
    } else {
        removeShaderDefine(name);
    }
}

void ShaderObject::removeShaderDefine(const std::string& name) { shaderDefines_.erase(name); }

bool ShaderObject::hasShaderDefine(const std::string& name) const {
    return shaderDefines_.find(name) != shaderDefines_.end();
}

void ShaderObject::clearShaderDefines() { shaderDefines_.clear(); }

void ShaderObject::addShaderExtension(const std::string& extName, bool enabled) {
    shaderExtensions_[extName] = enabled;
}

void ShaderObject::removeShaderExtension(const std::string& extName) {
    shaderExtensions_.erase(extName);
}

bool ShaderObject::hasShaderExtension(const std::string& extName) const {
    return shaderExtensions_.find(extName) != shaderExtensions_.end();
}

void ShaderObject::clearShaderExtensions() { shaderExtensions_.clear(); }

void ShaderObject::addSegment(ShaderSegment segment) {
    shaderSegments_.push_back(std::move(segment));
}

void ShaderObject::removeSegments(const std::string& name) {
    shaderSegments_.erase(
        std::remove_if(shaderSegments_.begin(), shaderSegments_.end(),
                       [&name](const ShaderSegment& segment) { return segment.name == name; }),
        shaderSegments_.end());
}

void ShaderObject::clearSegments() { shaderSegments_.clear(); }

void ShaderObject::addOutDeclaration(const std::string& name, int location,
                                     const std::string& type) {
    addOutDeclaration(OutDeclaration{name, location, type});
}

void ShaderObject::addOutDeclaration(const OutDeclaration& decl) {
    auto it = util::find_if(outDeclarations_,
                            [&](const OutDeclaration& elem) { return elem.name == decl.name; });
    if (it != outDeclarations_.end()) {
        *it = decl;
    } else {
        outDeclarations_.push_back(decl);
    }
}

auto ShaderObject::getOutDeclarations() const -> const std::vector<OutDeclaration>& {
    return outDeclarations_;
}

void ShaderObject::clearOutDeclarations() { outDeclarations_.clear(); }

void ShaderObject::ShaderObject::addInDeclaration(const std::string& name, int location,
                                                  const std::string& type) {
    addInDeclaration(InDeclaration{name, location, type});
}
void ShaderObject::addInDeclaration(const InDeclaration& decl) {
    auto it = util::find_if(inDeclarations_,
                            [&](const InDeclaration& elem) { return elem.name == decl.name; });
    if (it != inDeclarations_.end()) {
        *it = decl;
    } else {
        inDeclarations_.push_back(decl);
    }
}
void ShaderObject::clearInDeclarations() { inDeclarations_.clear(); }
auto ShaderObject::getInDeclarations() const -> const std::vector<InDeclaration>& {
    return inDeclarations_;
}

std::pair<std::string, size_t> ShaderObject::resolveLine(size_t line) const {
    return lnr_.resolveLine(line);
}

std::string ShaderObject::print(bool showSource, bool showPreprocess) {
    if (showPreprocess) {
        preprocess();  // Make sure sourceProcessed_ is set and up to date
        if (showSource) {
            std::string::size_type width = 0;
            for (auto l : lnr_) {
                std::string file = splitString(l.first, '/').back();
                width = std::max(width, file.length());
            }

            size_t i = 0;
            std::stringstream out;
            std::istringstream in(sourceProcessed_);

            std::string line;
            while (std::getline(in, line)) {
                const auto res = lnr_.resolveLine(i);
                const std::string file =
                    res.first.empty() ? "" : splitString(res.first, '/').back();
                const size_t lineNumber = res.second;

                out << std::left << std::setw(width + 1u) << file << std::right << std::setw(4)
                    << lineNumber << ": " << std::left << line << "\n";
                ++i;
            }
            return std::move(out).str();
        } else {
            return sourceProcessed_;
        }
    } else {
        if (showSource) {
            size_t lineNumber = 1;
            std::string line;
            std::stringstream out;
            std::istringstream in(resource_->source());
            while (std::getline(in, line)) {
                std::string file = resource_->key();

                out << std::left << std::setw(file.length() + 1u) << file << std::right
                    << std::setw(4) << lineNumber << ": " << std::left << line << "\n";
                ++lineNumber;
            }
            return out.str();
        } else {
            return resource_->source();
        }
    }
}

}  // namespace inviwo
