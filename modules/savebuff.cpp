/*
 * Copyright (C) 2004-2020 ZNC, see the NOTICE file for details.
 * Author: imaginos <imaginos@imaginos.net>
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Buffer Saving thing, incase your shit goes out while your out
 *
 * Its only as secure as your shell, the encryption only offers a slightly
 * better solution then plain text.
 */

#define REQUIRESSL

#include <znc/Chan.h>
#include <znc/User.h>
#include <znc/IRCNetwork.h>
#include <znc/FileUtils.h>
#include <znc/Query.h>

using std::set;
using std::vector;

#define LEGACY_VERIFICATION_TOKEN "::__:SAVEBUFF:__::"
#define CHAN_VERIFICATION_TOKEN "::__:CHANBUFF:__::"
#define QUERY_VERIFICATION_TOKEN "::__:QUERYBUFF:__::"
// this is basically plain text, but so is having the pass in the command line
// so *shrug*
// you could at least do something kind of cool like a bunch of unprintable text
#define CRYPT_LAME_PASS "::__:NOPASS:__::"
#define CRYPT_ASK_PASS "--ask-pass"

class CSaveBuff;

class CSaveBuffJob : public CTimer {
  public:
    CSaveBuffJob(CModule* pModule, unsigned int uInterval, unsigned int uCycles,
                 const CString& sLabel, const CString& sDescription)
        : CTimer(pModule, uInterval, uCycles, sLabel, sDescription) {}

    ~CSaveBuffJob() override {}

  protected:
    void RunJob() override;
};

class CSaveBuff : public CModule {
  public:
    MODCONSTRUCTOR(CSaveBuff) {
        m_bBootError = false;

        AddHelpCommand();
        AddCommand("SetPass", t_d("<password>"), t_d("Sets the password"),
                   [=](const CString& sLine) { OnSetPassCommand(sLine); });
        AddCommand("Replay", t_d("<buffer>"), t_d("Replays the buffer"),
                   [=](const CString& sLine) { OnReplayCommand(sLine); });
        AddCommand("Save", "", t_d("Saves all buffers"),
                   [=](const CString& sLine) { OnSaveCommand(sLine); });
    }
    ~CSaveBuff() override {
        if (!m_bBootError) {
            SaveBuffersToDisk();
        }
    }

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        if (sArgs == CRYPT_ASK_PASS) {
            char* pPass = getpass("Enter pass for savebuff: ");
            if (pPass)
                m_sPassword = CBlowfish::MD5(pPass);
            else {
                m_bBootError = true;
                sMessage = "Nothing retrieved from console. aborting";
            }
        } else if (sArgs.empty())
            m_sPassword = CBlowfish::MD5(CRYPT_LAME_PASS);
        else
            m_sPassword = CBlowfish::MD5(sArgs);

        AddTimer(new CSaveBuffJob(
            this, 60, 0, "SaveBuff",
            "Saves the current buffer to disk every 1 minute"));

        return (!m_bBootError);
    }

    bool OnBoot() override {
        CDir saveDir(GetSavePath());
        for (CFile* pFile : saveDir) {
            CString sName;
            CString sBuffer;

            EBufferType eType =
                DecryptBuffer(pFile->GetLongName(), sBuffer, sName);
            switch (eType) {
                case InvalidBuffer:
                    m_sPassword = "";
                    CUtils::PrintError("[" + GetModName() +
                                       ".so] Failed to Decrypt [" +
                                       pFile->GetLongName() + "]");
                    if (!sName.empty()) {
                        PutUser(":***!znc@znc.in PRIVMSG " + sName +
                                " :Failed to decrypt this buffer, did you "
                                "change the encryption pass?");
                    }
                    break;
                case ChanBuffer:
                    if (CChan* pChan = GetNetwork()->FindChan(sName)) {
                        BootStrap(pChan, sBuffer);
                    }
                    break;
                case QueryBuffer:
                    if (CQuery* pQuery = GetNetwork()->AddQuery(sName)) {
                        BootStrap(pQuery, sBuffer);
                    }
                    break;
                default:
                    break;
            }
        }
        return true;
    }

    template <typename T>
    void BootStrap(T* pTarget, const CString& sContent) {
        if (!pTarget->GetBuffer().IsEmpty())
            return;  // in this case the module was probably reloaded

        VCString vsLines;
        VCString::iterator it;

        sContent.Split("\n", vsLines);

        for (it = vsLines.begin(); it != vsLines.end(); ++it) {
            CString sLine(*it);
            sLine.Trim();
            if (sLine[0] == '@' && it + 1 != vsLines.end()) {
                CString sTimestamp = sLine.Token(0);
                sTimestamp.TrimLeft("@");
                timeval ts;
                ts.tv_sec = sTimestamp.Token(0, false, ",").ToLongLong();
                ts.tv_usec = sTimestamp.Token(1, false, ",").ToLong();

                CString sFormat = sLine.Token(1, true);

                CString sText(*++it);
                sText.Trim();

                pTarget->AddBuffer(sFormat, sText, &ts);
            } else {
                // Old format, escape the line and use as is.
                pTarget->AddBuffer(_NAMEDFMT(sLine));
            }
        }
    }

    void SaveBufferToDisk(const CBuffer& Buffer, const CString& sPath,
                          const CString& sHeader) {
        CFile File(sPath);
        CString sContent = sHeader + "\n";

        size_t uSize = Buffer.Size();
        for (unsigned int uIdx = 0; uIdx < uSize; uIdx++) {
            const CBufLine& Line = Buffer.GetBufLine(uIdx);
            timeval ts = Line.GetTime();
            sContent += "@" + CString(ts.tv_sec) + "," + CString(ts.tv_usec) +
                        " " + Line.GetFormat() + "\n" + Line.GetText() + "\n";
        }

        CBlowfish c(m_sPassword, BF_ENCRYPT);
        sContent = c.Crypt(sContent);

        if (File.Open(O_WRONLY | O_CREAT | O_TRUNC, 0600)) {
            File.Chmod(0600);
            File.Write(sContent);
        }
        File.Close();
    }

    void SaveBuffersToDisk() {
        if (!m_sPassword.empty()) {
            set<CString> ssPaths;

            const vector<CChan*>& vChans = GetNetwork()->GetChans();
            for (CChan* pChan : vChans) {
                CString sPath = GetPath(pChan->GetName());
                SaveBufferToDisk(pChan->GetBuffer(), sPath,
                                 CHAN_VERIFICATION_TOKEN + pChan->GetName());
                ssPaths.insert(sPath);
            }

            const vector<CQuery*>& vQueries = GetNetwork()->GetQueries();
            for (CQuery* pQuery : vQueries) {
                CString sPath = GetPath(pQuery->GetName());
                SaveBufferToDisk(pQuery->GetBuffer(), sPath,
                                 QUERY_VERIFICATION_TOKEN + pQuery->GetName());
                ssPaths.insert(sPath);
            }

            // cleanup leftovers ie. cleared buffers
            CDir saveDir(GetSavePath());
            for (CFile* pFile : saveDir) {
                if (ssPaths.count(pFile->GetLongName()) == 0) {
                    pFile->Delete();
                }
            }
        } else {
            PutModule(t_s(
                "Password is unset usually meaning the decryption failed. You "
                "can setpass to the appropriate pass and things should start "
                "working, or setpass to a new pass and save to reinstantiate"));
        }
    }

    void OnSetPassCommand(const CString& sCmdLine) {
        CString sArgs = sCmdLine.Token(1, true);

        if (sArgs.empty()) sArgs = CRYPT_LAME_PASS;

        PutModule(t_f("Password set to [{1}]")(sArgs));
        m_sPassword = CBlowfish::MD5(sArgs);
    }

    void OnModCommand(const CString& sCmdLine) override {
        CString sCommand = sCmdLine.Token(0);
        CString sArgs = sCmdLine.Token(1, true);

        if (sCommand.Equals("dumpbuff")) {
            // for testing purposes - hidden from help
            CString sFile;
            CString sName;
            if (DecryptBuffer(GetPath(sArgs), sFile, sName)) {
                VCString vsLines;
                sFile.Split("\n", vsLines);

                for (const CString& sLine : vsLines) {
                    PutModule("[" + sLine.Trim_n() + "]");
                }
            }
            PutModule("//!-- EOF " + sArgs);
        } else {
            HandleCommand(sCmdLine);
        }
    }

    void OnReplayCommand(const CString& sCmdLine) {
        CString sArgs = sCmdLine.Token(1, true);

        Replay(sArgs);
        PutModule(t_f("Replayed {1}")(sArgs));
    }

    void OnSaveCommand(const CString& sCmdLine) {
        SaveBuffersToDisk();
        PutModule("Done.");
    }

    void Replay(const CString& sBuffer) {
        CString sFile;
        CString sName;
        PutUser(":***!znc@znc.in PRIVMSG " + sBuffer + " :Buffer Playback...");
        if (DecryptBuffer(GetPath(sBuffer), sFile, sName)) {
            VCString vsLines;
            sFile.Split("\n", vsLines);

            for (const CString& sLine : vsLines) {
                PutUser(sLine.Trim_n());
            }
        }
        PutUser(":***!znc@znc.in PRIVMSG " + sBuffer + " :Playback Complete.");
    }

    CString GetPath(const CString& sTarget) const {
        CString sBuffer = GetUser()->GetUsername() + sTarget.AsLower();
        CString sRet = GetSavePath();
        sRet += "/" + CBlowfish::MD5(sBuffer, true);
        return (sRet);
    }

    CString FindLegacyBufferName(const CString& sPath) const {
        const vector<CChan*>& vChans = GetNetwork()->GetChans();
        for (CChan* pChan : vChans) {
            const CString& sName = pChan->GetName();
            if (GetPath(sName).Equals(sPath)) {
                return sName;
            }
        }
        return CString();
    }

  private:
    bool m_bBootError;
    CString m_sPassword;

    enum EBufferType {
        InvalidBuffer = 0,
        EmptyBuffer,
        ChanBuffer,
        QueryBuffer
    };

    EBufferType DecryptBuffer(const CString& sPath, CString& sBuffer,
                              CString& sName) {
        CString sContent;
        sBuffer = "";

        CFile File(sPath);

        if (sPath.empty() || !File.Open() || !File.ReadFile(sContent))
            return EmptyBuffer;

        File.Close();

        if (!sContent.empty()) {
            CBlowfish c(m_sPassword, BF_DECRYPT);
            sBuffer = c.Crypt(sContent);

            if (sBuffer.TrimPrefix(LEGACY_VERIFICATION_TOKEN)) {
                sName = FindLegacyBufferName(sPath);
                return ChanBuffer;
            } else if (sBuffer.TrimPrefix(CHAN_VERIFICATION_TOKEN)) {
                sName = sBuffer.FirstLine();
                if (sBuffer.TrimLeft(sName + "\n")) return ChanBuffer;
            } else if (sBuffer.TrimPrefix(QUERY_VERIFICATION_TOKEN)) {
                sName = sBuffer.FirstLine();
                if (sBuffer.TrimLeft(sName + "\n")) return QueryBuffer;
            }

            PutModule(t_f("Unable to decode Encrypted file {1}")(sPath));
            return InvalidBuffer;
        }
        return EmptyBuffer;
    }
};

void CSaveBuffJob::RunJob() {
    CSaveBuff* p = (CSaveBuff*)GetModule();
    p->SaveBuffersToDisk();
}

template <>
void TModInfo<CSaveBuff>(CModInfo& Info) {
    Info.SetWikiPage("savebuff");
    Info.SetHasArgs(true);
    Info.SetArgsHelpText(Info.t_s(
        "This user module takes up to one arguments. Either --ask-pass or the "
        "password itself (which may contain spaces) or nothing"));
}

NETWORKMODULEDEFS(CSaveBuff,
                  t_s("Stores channel and query buffers to disk, encrypted"))
