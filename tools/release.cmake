# The release workflow's manifest and notes (.github/workflows/release.yml),
# and what the unit target runs on a fixture. Writes <OUT>/manifest.json (the
# format of docs/PROTOCOL.md, "Release manifest") and <OUT>/notes.md (the
# tag's CHANGELOG section without its heading, its relative links made
# absolute). Fails, writing nothing, when the tag is malformed, CHANGELOG.md
# has no finished section for it, the image is missing, or the manifest
# would break a limit firmware 1.0.0 enforces (docs/PROTOCOL.md).
#
# DRY_RUN=ON (the workflow run by hand) lets a section still headed "not
# released yet" through with a warning; a tag run never does.
#
#   cmake -DROOT=<repo> -DTAG=v1.2.3 -DIMAGE=<app image> -DBOARD=m5stack-cores3
#         -DOUT=<dir> [-DCHANGELOG=<file>] [-DDRY_RUN=ON] -P release.cmake
cmake_minimum_required(VERSION 3.20)

foreach(var ROOT TAG IMAGE BOARD OUT)
    if("${${var}}" STREQUAL "")
        message(FATAL_ERROR "release: -D${var}= is missing")
    endif()
endforeach()
if("${CHANGELOG}" STREQUAL "")
    set(CHANGELOG ${ROOT}/CHANGELOG.md)
endif()

set(REPO_URL "https://github.com/veikkos/luaboard")
set(NOTES_MAX 120)    # bytes; the manifest's notes must fit the device's 128-byte buffer
set(CUT_MARK "..")    # ends a notes line that was cut

if(NOT TAG MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+(-.+)?$")
    message(FATAL_ERROR "release: the tag '${TAG}' is not vMAJOR.MINOR.PATCH (or vMAJOR.MINOR.PATCH-suffix)")
endif()
string(SUBSTRING "${TAG}" 1 -1 VERSION)
if(NOT EXISTS "${IMAGE}")
    message(FATAL_ERROR "release: the image ${IMAGE} does not exist")
endif()
if(NOT EXISTS "${CHANGELOG}")
    message(FATAL_ERROR "release: ${CHANGELOG} does not exist")
endif()

# ---------------------------------------------------------- the section
# The text from the heading "## <version>" to the next "## ". Lines are never
# split into a list here: the text holds ';' and '[' freely.
file(READ "${CHANGELOG}" text)
string(REPLACE "\r\n" "\n" text "${text}")
set(text "\n${text}\n")
set(rest "${text}")
set(heading "")
set(found FALSE)
while(NOT found)
    string(FIND "${rest}" "\n## ${VERSION}" at)
    if(at LESS 0)
        break()
    endif()
    string(LENGTH "\n## ${VERSION}" hlen)
    math(EXPR after "${at} + ${hlen}")
    string(SUBSTRING "${rest}" ${after} 1 next)
    string(SUBSTRING "${rest}" ${after} -1 rest)
    # "## 1.0.0" and "## 1.0.0 (date)", not "## 1.0.01" or "## 1.0.0-rc1"
    if(next STREQUAL " " OR next STREQUAL "\n")
        string(FIND "${rest}" "\n" eol)
        string(SUBSTRING "${rest}" 0 ${eol} heading)
        string(SUBSTRING "${rest}" ${eol} -1 rest)
        set(found TRUE)
    endif()
endwhile()
if(NOT found)
    message(FATAL_ERROR "release: ${CHANGELOG} has no '## ${VERSION}' section for the tag ${TAG}")
endif()
if(heading MATCHES "[Nn]ot released")
    if(DRY_RUN)
        message(WARNING "release: the '## ${VERSION}' heading still says 'not released yet' (fine for a dry run, a tag would fail)")
    else()
        message(FATAL_ERROR "release: the '## ${VERSION}' heading still says 'not released yet'; finish the section first")
    endif()
endif()
string(FIND "${rest}" "\n## " stop)
if(stop GREATER_EQUAL 0)
    string(SUBSTRING "${rest}" 0 ${stop} section)
else()
    set(section "${rest}")
endif()
string(STRIP "${section}" section)
if(section STREQUAL "")
    message(FATAL_ERROR "release: the '## ${VERSION}' section is empty")
endif()

# ---------------------------------------------------------- the notes line
# The first paragraph in one line, as plain text.
string(FIND "${section}" "\n\n" para_end)
if(para_end GREATER_EQUAL 0)
    string(SUBSTRING "${section}" 0 ${para_end} line)
else()
    set(line "${section}")
endif()
string(REPLACE "\n" " " line "${line}")
string(REPLACE "\t" " " line "${line}")
string(REPLACE "`" "" line "${line}")
string(REGEX REPLACE "\\[([^]]*)\\]\\([^)]*\\)" "\\1" line "${line}")
string(REGEX REPLACE "  +" " " line "${line}")
string(STRIP "${line}" line)

# At most NOTES_MAX bytes, cut at a word and ended with CUT_MARK. (CMake's
# strings count bytes.)
string(LENGTH "${line}" len)
if(len GREATER NOTES_MAX)
    string(LENGTH "${CUT_MARK}" mark_len)
    math(EXPR keep "${NOTES_MAX} - ${mark_len}")
    math(EXPR window "${keep} + 1")
    string(SUBSTRING "${line}" 0 ${window} cut)
    string(FIND "${cut}" " " sp REVERSE)
    if(sp GREATER 0)
        string(SUBSTRING "${cut}" 0 ${sp} line)
    else()
        # One long word: cut it, then drop a UTF-8 character the cut split.
        string(SUBSTRING "${line}" 0 ${keep} line)
        string(HEX "${line}" hex)
        string(LENGTH "${hex}" hexlen)
        set(pos ${hexlen})
        set(drop 0)
        while(pos GREATER 0)
            math(EXPR pos "${pos} - 2")
            string(SUBSTRING "${hex}" ${pos} 2 b)
            math(EXPR v "0x${b}")
            if(v GREATER_EQUAL 192)                    # a lead byte: 11xxxxxx
                if(v GREATER_EQUAL 240)
                    set(need 4)
                elseif(v GREATER_EQUAL 224)
                    set(need 3)
                else()
                    set(need 2)
                endif()
                math(EXPR have "(${hexlen} - ${pos}) / 2")
                if(have LESS need)
                    set(drop ${pos})
                endif()
                break()
            elseif(v LESS 128)                         # ASCII: whole
                break()
            endif()                                    # else a continuation byte: look back
        endwhile()
        if(drop GREATER 0)
            math(EXPR bytes "${drop} / 2")
            string(SUBSTRING "${line}" 0 ${bytes} line)
        endif()
    endif()
    string(STRIP "${line}" line)
    string(REGEX REPLACE "[ ,;:.]+$" "" line "${line}")
    string(APPEND line "${CUT_MARK}")
endif()
string(REPLACE "\\" "\\\\" notes "${line}")
string(REPLACE "\"" "\\\"" notes "${notes}")

# ---------------------------------------------------------- the manifest
file(READ "${ROOT}/app/src/app_config.h" config)
if(NOT config MATCHES "#define APP_LUA_API[ \t]+([0-9]+)")
    message(FATAL_ERROR "release: APP_LUA_API not found in app/src/app_config.h")
endif()
set(lua_api ${CMAKE_MATCH_1})
if(NOT config MATCHES "#define APP_LUA_API_MIN[ \t]+([0-9]+)")
    message(FATAL_ERROR "release: APP_LUA_API_MIN not found in app/src/app_config.h")
endif()
set(lua_api_min ${CMAKE_MATCH_1})

if(NOT config MATCHES "#define UPDATE_MANIFEST_MAX[ \t]+([0-9]+)")
    message(FATAL_ERROR "release: UPDATE_MANIFEST_MAX not found in app/src/app_config.h")
endif()
set(manifest_max ${CMAKE_MATCH_1})
if(lua_api_min GREATER lua_api)
    message(FATAL_ERROR "release: APP_LUA_API_MIN (${lua_api_min}) is above APP_LUA_API (${lua_api})")
endif()

file(SIZE "${IMAGE}" size)
file(SHA256 "${IMAGE}" sha256)

# ---------------------------------------------------------- limits
# What firmware 1.0.0 accepts, forever (docs/PROTOCOL.md, "Release manifest").
set(url "${REPO_URL}/releases/download/${TAG}/luaboard.bin")
string(LENGTH "${TAG}" n)
if(n GREATER_EQUAL 32)
    message(FATAL_ERROR "release: the tag ${TAG} is ${n} bytes, a manifest's version must be under 32")
endif()
if(NOT BOARD MATCHES "^[A-Za-z0-9._-]+$")
    message(FATAL_ERROR "release: the board name '${BOARD}' must be letters, digits, '.', '_' or '-'")
endif()
string(LENGTH "${BOARD}" n)
if(n GREATER_EQUAL 32)
    message(FATAL_ERROR "release: the board name ${BOARD} is ${n} bytes, it must be under 32")
endif()
string(LENGTH "${url}" n)
if(n GREATER_EQUAL 256)
    message(FATAL_ERROR "release: the image's address is ${n} bytes, it must be under 256: ${url}")
endif()
if(NOT url MATCHES "^https://")
    message(FATAL_ERROR "release: the image's address isn't https: ${url}")
endif()
if(size EQUAL 0 OR size GREATER 999999999)
    message(FATAL_ERROR "release: the image is ${size} bytes, it must be 1 to 999999999")
endif()
if(lua_api GREATER 999999999)
    message(FATAL_ERROR "release: APP_LUA_API ${lua_api} is too big for a manifest")
endif()

set(manifest "{
  \"format\": 1,
  \"version\": \"${TAG}\",
  \"board\": \"${BOARD}\",
  \"url\": \"${url}\",
  \"size\": ${size},
  \"sha256\": \"${sha256}\",
  \"lua_api\": ${lua_api},
  \"lua_api_min\": ${lua_api_min},
  \"notes\": \"${notes}\"
}
")
string(LENGTH "${manifest}" n)
if(n GREATER manifest_max)
    message(FATAL_ERROR "release: the manifest is ${n} bytes, the device takes at most ${manifest_max} (UPDATE_MANIFEST_MAX)")
endif()

# ---------------------------------------------------------- the release notes
# GitHub shows them on the release's page, where a relative link would go
# nowhere: "](path" becomes "](<repo>/blob/<tag>/path". Links with a scheme
# (https:, mailto:) and anchors (#...) stay.
set(rest "${section}")
set(notes_md "")
while(TRUE)
    string(FIND "${rest}" "](" at)
    if(at LESS 0)
        break()
    endif()
    math(EXPR after "${at} + 2")
    string(SUBSTRING "${rest}" 0 ${after} head)
    string(APPEND notes_md "${head}")
    string(SUBSTRING "${rest}" ${after} -1 rest)
    if(NOT rest MATCHES "^([A-Za-z][A-Za-z0-9+.-]*:|#|<|\\)|[ \t\n]|$)")
        string(REGEX REPLACE "^(\\./|/)+" "" rest "${rest}")
        string(APPEND notes_md "${REPO_URL}/blob/${TAG}/")
    endif()
endwhile()
string(APPEND notes_md "${rest}")

file(MAKE_DIRECTORY "${OUT}")
file(WRITE "${OUT}/manifest.json" "${manifest}")
file(WRITE "${OUT}/notes.md" "${notes_md}\n")
message(STATUS "release: ${TAG}, ${size} bytes, notes: ${line}")
