//
// Copyright (C) 2017 Privacore ApS - https://www.privacore.com
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU Affero General Public License as
// published by the Free Software Foundation, either version 3 of the
// License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU Affero General Public License for more details.
//
// You should have received a copy of the GNU Affero General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
// License TL;DR: If you change this file, you must publish your changes.
//
#ifndef FXCHECKSPAM_H_
#define FXCHECKSPAM_H_

#include "FxTermCheckList.h"
#include <inttypes.h>
#include <stddef.h>
#include <string>

class TokenizerResult;

class CheckSpam {
public:
	CheckSpam(XmlDoc *xd, bool debug=false);
	~CheckSpam();

	bool init();
	bool isDocSpam();
	int32_t getScore();
	int32_t getNumUniqueMatchedWords();
	int32_t getNumUniqueMatchedPhrases();
	int32_t getNumWordsChecked();
	bool hasEmptyDocumentBody();
	const char *getReason();
	const char *getDebugInfo();

private:
	Url *m_url;
	Xml *m_xml;
	TokenizerResult *m_tokenizerResult;
	Phrases *m_phrases;

	char *m_debbuf;
	int m_debbufUsed;
	int m_debbufSize;

	std::string m_reason;
	int32_t m_docMatchScore;
	int32_t m_numUniqueMatchedWords;
	int32_t m_numUniqueMatchedPhrases;
	int32_t m_numWordsChecked;
	bool m_emptyDocumentBody;
	bool m_resultValid;
	bool m_result;
};

extern TermCheckList g_checkSpamList;


#endif
