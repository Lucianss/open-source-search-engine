#ifndef GB_TITLERECVERSION_H
#define GB_TITLERECVERSION_H

// Starting version when Gigablast was open-sourced
//#define TITLEREC_CURRENT_VERSION 120

// better parsing of <script> tags
//#define TITLEREC_CURRENT_VERSION 121

// BR 20160106. New version that eliminates values in posdb that we do not need.
// we also stop decoding &amp; &gt; &lt; to avoid losing information
//#define TITLEREC_CURRENT_VERSION  122

// normalize url encoded url (url encode, strip params)
//#define TITLEREC_CURRENT_VERSION    123

// strip more parameters
//#define TITLEREC_CURRENT_VERSION    124

// strip ascii tab & newline from url
// store m_indexCode in TitleRec
//#define TITLEREC_CURRENT_VERSION    125

// new adult detection
#define TITLEREC_CURRENT_VERSION	126

#endif // GB_TITLERECVERSION_H
