/*
 * Copyright (C) 2004-2020 ZNC, see the NOTICE file for details.
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

#include <znc/Modules.h>
#include <znc/Chan.h>
#include <time.h>

class CCtcpFloodMod : public CModule {
  public:
    MODCONSTRUCTOR(CCtcpFloodMod) {
        AddHelpCommand();
        AddCommand("Secs", t_d("<limit>"), t_d("Set seconds limit"),
                   [=](const CString& sLine) { OnSecsCommand(sLine); });
        AddCommand("Lines", t_d("<limit>"), t_d("Set lines limit"),
                   [=](const CString& sLine) { OnLinesCommand(sLine); });
        AddCommand("Show", "", t_d("Show the current limits"),
                   [=](const CString& sLine) { OnShowCommand(sLine); });
    }

    ~CCtcpFloodMod() override {}

    void Save() {
        // We save the settings twice because the module arguments can
        // be more easily edited via webadmin, while the SetNV() stuff
        // survives e.g. /msg *status reloadmod ctcpflood.
        SetNV("secs", CString(m_iThresholdSecs));
        SetNV("msgs", CString(m_iThresholdMsgs));

        SetArgs(CString(m_iThresholdMsgs) + " " + CString(m_iThresholdSecs));
    }

    bool OnLoad(const CString& sArgs, CString& sMessage) override {
        m_iThresholdMsgs = sArgs.Token(0).ToUInt();
        m_iThresholdSecs = sArgs.Token(1).ToUInt();

        if (m_iThresholdMsgs == 0 || m_iThresholdSecs == 0) {
            m_iThresholdMsgs = GetNV("msgs").ToUInt();
            m_iThresholdSecs = GetNV("secs").ToUInt();
        }

        if (m_iThresholdSecs == 0) m_iThresholdSecs = 2;
        if (m_iThresholdMsgs == 0) m_iThresholdMsgs = 4;

        Save();

        return true;
    }

    EModRet Message(const CNick& Nick, const CString& sMessage) {
        // We never block /me, because it doesn't cause a reply
        if (sMessage.Token(0).Equals("ACTION")) return CONTINUE;

        if (m_tLastCTCP + m_iThresholdSecs < time(nullptr)) {
            m_tLastCTCP = time(nullptr);
            m_iNumCTCP = 0;
        }

        m_iNumCTCP++;

        if (m_iNumCTCP < m_iThresholdMsgs)
            return CONTINUE;
        else if (m_iNumCTCP == m_iThresholdMsgs)
            PutModule(t_f("Limit reached by {1}, blocking all CTCP")(
                Nick.GetHostMask()));

        // Reset the timeout so that we continue blocking messages
        m_tLastCTCP = time(nullptr);

        return HALT;
    }

    EModRet OnPrivCTCP(CNick& Nick, CString& sMessage) override {
        return Message(Nick, sMessage);
    }

    EModRet OnChanCTCP(CNick& Nick, CChan& Channel,
                       CString& sMessage) override {
        return Message(Nick, sMessage);
    }

    void OnSecsCommand(const CString& sCommand) {
        const CString& sArg = sCommand.Token(1, true);

        if (sArg.empty()) {
            PutModule(t_s("Usage: Secs <limit>"));
            return;
        }

        m_iThresholdSecs = sArg.ToUInt();
        if (m_iThresholdSecs == 0) m_iThresholdSecs = 1;

        OnShowCommand("");
        Save();
    }

    void OnLinesCommand(const CString& sCommand) {
        const CString& sArg = sCommand.Token(1, true);

        if (sArg.empty()) {
            PutModule(t_s("Usage: Lines <limit>"));
            return;
        }

        m_iThresholdMsgs = sArg.ToUInt();
        if (m_iThresholdMsgs == 0) m_iThresholdMsgs = 2;

        OnShowCommand("");
        Save();
    }

    void OnShowCommand(const CString& sCommand) {
        CString sMsgs = t_p("1 CTCP message", "{1} CTCP messages",
                            m_iThresholdMsgs)(m_iThresholdMsgs);
        CString sSecs = t_p("every second", "every {1} seconds",
                            m_iThresholdSecs)(m_iThresholdSecs);
        PutModule(t_f("Current limit is {1} {2}")(sMsgs, sSecs));
    }

  private:
    time_t m_tLastCTCP = 0;
    unsigned int m_iNumCTCP = 0;

    time_t m_iThresholdSecs{};
    unsigned int m_iThresholdMsgs{};
};

template <>
void TModInfo<CCtcpFloodMod>(CModInfo& Info) {
    Info.SetWikiPage("ctcpflood");
    Info.SetHasArgs(true);
    Info.SetArgsHelpText(Info.t_s(
        "This user module takes none to two arguments. The first argument is "
        "the number of lines after which the flood-protection is triggered. "
        "The second argument is the time (sec) to in which the number of lines "
        "is reached. The default setting is 4 CTCPs in 2 seconds"));
}

USERMODULEDEFS(CCtcpFloodMod, t_s("Don't forward CTCP floods to clients"))
