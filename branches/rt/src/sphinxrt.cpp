//
// $Id$
//

#include "sphinx.h"
#include "sphinxint.h"
#include "sphinxrt.h"

//////////////////////////////////////////////////////////////////////////

#define	COMPRESSED_DOCLIST		1
#define	COMPRESSED_WORDLIST		1
#define	WORDID_MAX				0xffffffffUL; // !COMMIT might be qword

//////////////////////////////////////////////////////////////////////////

static inline void ZipDword ( CSphVector<BYTE> & dOut, DWORD uValue )
{
	do 
	{
		BYTE bOut = BYTE( uValue & 0x7f );
		uValue >>= 7;
		if ( uValue )
			bOut |= 0x80;
		dOut.Add ( bOut );
	} while ( uValue );
}


static inline const BYTE * UnzipDword ( DWORD * pValue, const BYTE * pIn )
{
	DWORD uValue = 0;
	BYTE bIn;
	int iOff = 0;

	do 
	{
		bIn = *pIn++;
		uValue += ( bIn & 0x7f )<<iOff;
		iOff += 7;
	} while ( bIn & 0x80 );

	*pValue = uValue;
	return pIn;
}

//////////////////////////////////////////////////////////////////////////

struct CmpHit_fn
{
	inline int operator () ( const CSphWordHit & a, const CSphWordHit & b )
	{
		return 	( a.m_iWordID < b.m_iWordID ) ||
			( a.m_iWordID == b.m_iWordID && a.m_iDocID < b.m_iDocID ) || 
			( a.m_iWordID == b.m_iWordID && a.m_iDocID == b.m_iDocID && a.m_iWordPos < b.m_iWordPos );
	}
};


struct RtDoc_t
{
	SphDocID_t					m_uDocID;	///< my document id
	DWORD						m_uFields;	///< fields mask
	DWORD						m_uHits;	///< hit count
	DWORD						m_uHit;		///< either index into segment hits, or the only hit itself (if hit count is 1)
};


struct RtWord_t
{
	SphWordID_t					m_uWordID;	///< my keyword id
	DWORD						m_uDocs;	///< document count (for stats and/or BM25)
	DWORD						m_uHits;	///< hit count (for stats and/or BM25)
	DWORD						m_uDoc;		///< index into segment docs
};


struct RtSegment_t
{
#ifndef NDEBUG
	int							m_iTag;
	static int					m_iSegments;
#endif

#if COMPRESSED_WORDLIST
	CSphVector<BYTE>			m_dWords;
#else
	CSphVector<RtWord_t>		m_dWords;
#endif

#if COMPRESSED_DOCLIST
	CSphVector<BYTE>			m_dDocs;
#else
	CSphVector<RtDoc_t>			m_dDocs;
#endif
	CSphVector<DWORD>			m_dHits;

	int							m_iDocs;

	RtSegment_t ()
	{
#ifndef NDEBUG
		m_iTag = m_iSegments++;
#endif
		m_iDocs = 0;
	}

	int GetSizeBytes () const
	{
		return
			m_dWords.GetLength()*sizeof(m_dWords[0]) +
			m_dDocs.GetLength()*sizeof(m_dDocs[0]) +
			m_dHits.GetLength()*sizeof(m_dHits[0]);
	}

	int GetMergeFactor () const
	{
		return m_iDocs;
	}

public:
#if COMPRESSED_DOCLIST
	void ZipDoc ( const RtDoc_t & tDoc )
	{
		ZipDword ( m_dDocs, tDoc.m_uDocID ); // !COMMIT might be qword
		ZipDword ( m_dDocs, tDoc.m_uFields );
		ZipDword ( m_dDocs, tDoc.m_uHits );
		if ( tDoc.m_uHits==1 )
		{
			ZipDword ( m_dDocs, tDoc.m_uHit & 0xffffffUL );
			ZipDword ( m_dDocs, tDoc.m_uHit>>24 );
		} else
			ZipDword ( m_dDocs, tDoc.m_uHit );
	}
#else
	void ZipDoc ( const RtDoc_t & tDoc )
	{
		m_dDocs.Add ( tDoc );
	}
#endif

	DWORD ZipDocPtr () const
	{
		return m_dDocs.GetLength();
	}
};


#if COMPRESSED_DOCLIST

struct RtDocIterator_t
{
	const BYTE *	m_pDocs;
	int				m_iLeft;
	RtDoc_t			m_tDoc;

	explicit RtDocIterator_t ( const RtSegment_t * pSeg, const RtWord_t & tWord )
	{
		m_pDocs = &pSeg->m_dDocs[0] + tWord.m_uDoc;
		m_iLeft = tWord.m_uDocs;
	}

	const RtDoc_t * UnzipDoc ()
	{
		if ( !m_iLeft )
			return NULL;

		const BYTE * pIn = m_pDocs;
		pIn = UnzipDword ( &m_tDoc.m_uDocID, pIn ); // !COMMIT might be qword
		pIn = UnzipDword ( &m_tDoc.m_uFields, pIn );
		pIn = UnzipDword ( &m_tDoc.m_uHits, pIn );
		if ( m_tDoc.m_uHits==1 )
		{
			DWORD a, b;
			pIn = UnzipDword ( &a, pIn );
			pIn = UnzipDword ( &b, pIn );
			m_tDoc.m_uHit = a + ( b<<24 );
		} else
			pIn = UnzipDword ( &m_tDoc.m_uHit, pIn );
		m_pDocs = pIn;

		m_iLeft--;
		return &m_tDoc;
	}
};

#else

struct RtDocIterator_t
{
	const RtDoc_t *	m_pDocs;
	int				m_iPos;
	int				m_iMax;

	explicit RtDocIterator_t ( const RtSegment_t * pSeg, const RtWord_t & tWord )
	{
		m_pDocs = &pSeg->m_dDocs[0];
		m_iPos = tWord.m_uDoc;
		m_iMax = tWord.m_uDoc + tWord.m_uDocs;
	}

	const RtDoc_t * UnzipDoc ()
	{
		return m_iPos<m_iMax ? m_pDocs + m_iPos++ : NULL;
	}
};

#endif // COMPRESSED_DOCLIST


#if COMPRESSED_WORDLIST

struct RtWordWriter_t
{
	CSphVector<BYTE> *	m_pWords;
	SphWordID_t			m_uLastWordID;
	DWORD				m_uLastDoc;

	explicit RtWordWriter_t ( RtSegment_t * pSeg )
		: m_pWords ( &pSeg->m_dWords )
		, m_uLastWordID ( 0 )
		, m_uLastDoc ( 0 )
	{}

	void ZipWord ( const RtWord_t & tWord )
	{
		CSphVector<BYTE> & tWords = *m_pWords;
		ZipDword ( tWords, tWord.m_uWordID - m_uLastWordID ); // !COMMIT might be qword
		ZipDword ( tWords, tWord.m_uDocs );
		ZipDword ( tWords, tWord.m_uHits );
		ZipDword ( tWords, tWord.m_uDoc - m_uLastDoc );
		m_uLastWordID = tWord.m_uWordID;
		m_uLastDoc = tWord.m_uDoc;
	}
};


struct RtWordReader_t
{
	const BYTE *	m_pCur;
	const BYTE *	m_pMax;
	RtWord_t		m_tWord;	

	explicit RtWordReader_t ( const RtSegment_t * pSeg )
	{
		m_pCur = &pSeg->m_dWords[0];
		m_pMax = m_pCur + pSeg->m_dWords.GetLength();

		m_tWord.m_uWordID = 0;
		m_tWord.m_uDoc = 0;
	}

	const RtWord_t * UnzipWord ()
	{
		if ( m_pCur>=m_pMax )
			return NULL;

		const BYTE * pIn = m_pCur;
		DWORD uDeltaID, uDeltaDoc;
		pIn = UnzipDword ( &uDeltaID, pIn ); // !COMMIT might be qword
		pIn = UnzipDword ( &m_tWord.m_uDocs, pIn );
		pIn = UnzipDword ( &m_tWord.m_uHits, pIn );
		pIn = UnzipDword ( &uDeltaDoc, pIn );
		m_pCur = pIn;

		m_tWord.m_uWordID += uDeltaID;
		m_tWord.m_uDoc += uDeltaDoc;
		return &m_tWord;
	}
};

#else

struct RtWordWriter_t
{
	CSphVector<RtWord_t> *	m_pWords;

	explicit				RtWordWriter_t ( RtSegment_t * pSeg )	: m_pWords ( &pSeg->m_dWords ) {}
	void					ZipWord ( const RtWord_t & tWord )		{ m_pWords->Add ( tWord ); }
};


struct RtWordReader_t
{
	const RtWord_t *	m_pCur;
	const RtWord_t *	m_pMax;

	explicit RtWordReader_t ( const RtSegment_t * pSeg )
	{
		m_pCur = &pSeg->m_dWords[0];
		m_pMax = m_pCur + pSeg->m_dWords.GetLength();
	}

	const RtWord_t * UnzipWord ()
	{
		return m_pCur<m_pMax ? m_pCur++ : NULL;
	}
};

#endif // COMPRESSED_WORDLIST


#ifndef NDEBUG
int RtSegment_t::m_iSegments = 0;
#endif


struct RtIndex_t : public ISphRtIndex
{
	CSphVector<CSphWordHit>		m_dAccum;
	CSphVector<RtSegment_t*>	m_pSegments;
	int							m_iAccumDocs;

	explicit					RtIndex_t ();
	virtual						~RtIndex_t ();

	void						AddDocument ( const CSphVector<CSphWordHit> & dHits );
	void						Commit ();

	RtSegment_t *				CreateSegment ();
	RtSegment_t *				MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2 );
	const RtWord_t *			CopyWord ( RtSegment_t * pDst, const RtSegment_t * pSrc, const RtWord_t * pWord, RtWordWriter_t & tOut, RtWordReader_t & tIn );
	void						MergeWord ( RtSegment_t * pDst, const RtSegment_t * pSrc1, const RtWord_t * pWord1, const RtSegment_t * pSrc2, const RtWord_t * pWord2, RtWordWriter_t & tOut );
	void						CopyDoc ( RtSegment_t * pSeg, RtWord_t * pWord, const RtSegment_t * pSrc, const RtDoc_t * pDoc );

	void						DumpToDisk ( const char * sFilename );
};


RtIndex_t::RtIndex_t ()
{
	m_dAccum.Reserve ( 2*1024*1024 );
	m_iAccumDocs = 0;
}


RtIndex_t::~RtIndex_t ()
{
	ARRAY_FOREACH ( i, m_pSegments )
		SafeDelete ( m_pSegments[i] );
}


void RtIndex_t::AddDocument ( const CSphVector<CSphWordHit> & dHits )
{
	ARRAY_FOREACH ( i, dHits )
		m_dAccum.Add ( dHits[i] );
	m_iAccumDocs++;
}


RtSegment_t * RtIndex_t::CreateSegment ()
{
	RtSegment_t * pSeg = new RtSegment_t ();

	CSphWordHit tClosingHit;
	tClosingHit.m_iWordID = WORDID_MAX;
	tClosingHit.m_iDocID = DOCID_MAX;
	tClosingHit.m_iWordPos = 1;
	m_dAccum.Add ( tClosingHit );
	m_dAccum.Sort ( CmpHit_fn() );

	RtDoc_t tDoc;
	tDoc.m_uDocID = 0;
	tDoc.m_uFields = 0;
	tDoc.m_uHits = 0;
	tDoc.m_uHit = 0;

	RtWord_t tWord;
	tWord.m_uWordID = 0;
	tWord.m_uDocs = 0;
	tWord.m_uHits = 0;
	tWord.m_uDoc = 0;

	RtWordWriter_t tOut ( pSeg );
	ARRAY_FOREACH ( i, m_dAccum )
	{
		const CSphWordHit & tHit = m_dAccum[i];

		// new keyword or doc; flush current doc
		if ( tHit.m_iWordID!=tWord.m_uWordID || tHit.m_iDocID!=tDoc.m_uDocID )
		{
			if ( tDoc.m_uDocID )
			{
				tWord.m_uDocs++;
				tWord.m_uHits += tDoc.m_uHits;

				if ( tDoc.m_uHits==1 )
					tDoc.m_uHit = pSeg->m_dHits.Pop();

				pSeg->ZipDoc ( tDoc );
				tDoc.m_uFields = 0;
				tDoc.m_uHits = 0;
				tDoc.m_uHit = pSeg->m_dHits.GetLength();
			}
			tDoc.m_uDocID = tHit.m_iDocID;
		}

		// new keyword; flush current keyword
		if ( tHit.m_iWordID!=tWord.m_uWordID )
		{
			if ( tWord.m_uWordID )
				tOut.ZipWord (  tWord );

			tWord.m_uWordID = tHit.m_iWordID;
			tWord.m_uDocs = 0;
			tWord.m_uHits = 0;
			tWord.m_uDoc = pSeg->ZipDocPtr();
		}

		// just a new hit
		pSeg->m_dHits.Add ( tHit.m_iWordPos );
		tDoc.m_uFields |= 1UL << ( tHit.m_iWordPos>>24 ); // !COMMIT HIT2LCS()
		tDoc.m_uHits++;
	}

	m_dAccum.Resize ( 0 );
	pSeg->m_iDocs = m_iAccumDocs;
	m_iAccumDocs = 0;
	return pSeg;
}


const RtWord_t * RtIndex_t::CopyWord ( RtSegment_t * pDst, const RtSegment_t * pSrc, const RtWord_t * pWord, RtWordWriter_t & tOut, RtWordReader_t & tIn )
{
	// append word to the dictionary
	RtWord_t tNewWord = *pWord;
	tNewWord.m_uDoc = pDst->ZipDocPtr();
	tOut.ZipWord ( tNewWord );

	// copy docs
	DWORD uStartHit = 1;
	DWORD uEndHit = 0;

	DWORD uHit = pDst->m_dHits.GetLength();
	RtDocIterator_t tDocIt ( pSrc, *pWord );
	for ( ;; )
	{
		const RtDoc_t * pDoc = tDocIt.UnzipDoc();
		if ( !pDoc )
			break;

		RtDoc_t tDoc = *pDoc;
		if ( tDoc.m_uHits>1 )
		{
			if ( uStartHit>uEndHit )
			{
				uStartHit = tDoc.m_uHit;
				assert ( (int)uStartHit < pSrc->m_dHits.GetLength() );
			}

			uEndHit = tDoc.m_uHit + tDoc.m_uHits;
			assert ( (int)uEndHit <= pSrc->m_dHits.GetLength() );

			tDoc.m_uHit = uHit;
			uHit += tDoc.m_uHits;
		}

		pDst->ZipDoc ( tDoc );
	}

	// copy hits
	for ( DWORD i=uStartHit; i<uEndHit; i++ )
		pDst->m_dHits.Add ( pSrc->m_dHits[i] );

	// move forward
	return tIn.UnzipWord ();
}


void RtIndex_t::CopyDoc ( RtSegment_t * pSeg, RtWord_t * pWord, const RtSegment_t * pSrc, const RtDoc_t * pDoc )
{
	pWord->m_uDocs++;
	pWord->m_uHits += pDoc->m_uHits;

	RtDoc_t tDoc = *pDoc;

	if ( tDoc.m_uHits>1 )
	{
		tDoc.m_uHit = pSeg->m_dHits.GetLength();
		for ( DWORD i=pDoc->m_uHit; i<pDoc->m_uHit+pDoc->m_uHits; i++ )
			pSeg->m_dHits.Add ( pSrc->m_dHits[i] );
	}

	pSeg->ZipDoc ( tDoc );
}


void RtIndex_t::MergeWord ( RtSegment_t * pSeg, const RtSegment_t * pSrc1, const RtWord_t * pWord1, const RtSegment_t * pSrc2, const RtWord_t * pWord2, RtWordWriter_t & tOut )
{
	assert ( pWord1->m_uWordID==pWord2->m_uWordID );

	RtWord_t tWord;
	tWord.m_uWordID = pWord1->m_uWordID;
	tWord.m_uDocs = 0;
	tWord.m_uHits = 0;
	tWord.m_uDoc = pSeg->ZipDocPtr();

	RtDocIterator_t tIn1 ( pSrc1, *pWord1 );
	RtDocIterator_t tIn2 ( pSrc2, *pWord2 );
	const RtDoc_t * pDoc1 = tIn1.UnzipDoc();
	const RtDoc_t * pDoc2 = tIn2.UnzipDoc();

	for ( ;; )
	{
		// copy non-matching docs
		while ( pDoc1 && pDoc2 && pDoc1->m_uDocID!=pDoc2->m_uDocID )
		{
			if ( pDoc1->m_uDocID < pDoc2->m_uDocID )
			{
				CopyDoc ( pSeg, &tWord, pSrc1, pDoc1 );
				pDoc1 = tIn1.UnzipDoc();
			} else
			{
				CopyDoc ( pSeg, &tWord, pSrc2, pDoc2 );
				pDoc2 = tIn2.UnzipDoc();
			}
		}

		if ( !pDoc1 || !pDoc2 )
			break;

		// merge matching docs
		assert ( pDoc1 && pDoc2 && pDoc1->m_uDocID==pDoc2->m_uDocID );
		assert ( 0 );
		sphDie ( "!COMMIT not implemented yet" );
	}

	assert ( !pDoc1 || !pDoc2 );
	while ( pDoc1 )
	{
		CopyDoc ( pSeg, &tWord, pSrc1, pDoc1 );
		pDoc1 = tIn1.UnzipDoc();
	}
	while ( pDoc2 )
	{
		CopyDoc ( pSeg, &tWord, pSrc2, pDoc2 );
		pDoc2 = tIn2.UnzipDoc();
	}

	tOut.ZipWord ( tWord );
}


RtSegment_t * RtIndex_t::MergeSegments ( const RtSegment_t * pSeg1, const RtSegment_t * pSeg2 )
{
	RtSegment_t * pSeg = new RtSegment_t ();
	pSeg->m_iDocs = pSeg1->m_iDocs + pSeg2->m_iDocs; // !COMMIT incorrect because of dupes

	RtWordWriter_t tOut ( pSeg );
	RtWordReader_t tIn1 ( pSeg1 );
	RtWordReader_t tIn2 ( pSeg2 );
	const RtWord_t * pWords1 = tIn1.UnzipWord ();
	const RtWord_t * pWords2 = tIn2.UnzipWord ();

	// merge while there are common words
	for ( ;; )
	{
		while ( pWords1 && pWords2 && pWords1->m_uWordID!=pWords2->m_uWordID )
			if ( pWords1->m_uWordID < pWords2->m_uWordID )
				pWords1 = CopyWord ( pSeg, pSeg1, pWords1, tOut, tIn1 );
			else
				pWords2 = CopyWord ( pSeg, pSeg2, pWords2, tOut, tIn2 );

		if ( !pWords1 || !pWords2 )
			break;

		assert ( pWords1 && pWords2 && pWords1->m_uWordID==pWords2->m_uWordID );
		MergeWord ( pSeg, pSeg1, pWords1, pSeg2, pWords2, tOut );
		pWords1 = tIn1.UnzipWord();
		pWords2 = tIn2.UnzipWord();
	}

	// copy tails
	while ( pWords1 ) pWords1 = CopyWord ( pSeg, pSeg1, pWords1, tOut, tIn1 );
	while ( pWords2 ) pWords2 = CopyWord ( pSeg, pSeg2, pWords2, tOut, tIn2 );
	return pSeg;
}


struct CmpSegments_fn
{
	inline int operator () ( const RtSegment_t * a, const RtSegment_t * b )
	{
		return a->GetMergeFactor() > b->GetMergeFactor();
	}
};


void RtIndex_t::Commit ()
{
	if ( !m_dAccum.GetLength() )
		return;

	CSphVector<RtSegment_t*> dSegments;
	dSegments = m_pSegments;
	dSegments.Add ( CreateSegment() );

	CSphVector<RtSegment_t*> dToKill;

	const int MAX_SEGMENTS = 8;
	for ( ;; )
	{
		dSegments.Sort ( CmpSegments_fn() );

		// unconditionally merge if there's too much segments now
		// conditionally merge if smallest segment has grown too large
		// otherwise, we're done
		int iLen = dSegments.GetLength();
		if (!( iLen>MAX_SEGMENTS || ( iLen>=2 && dSegments[iLen-1]->GetMergeFactor()*2 > dSegments[iLen-2]->GetMergeFactor() ) ))
			break;

		RtSegment_t * pA = dSegments.Pop();
		RtSegment_t * pB = dSegments.Pop();
		dSegments.Add ( MergeSegments ( pA, pB ) );
		dToKill.Add ( pA );
		dToKill.Add ( pB );
	}

	Swap ( m_pSegments, dSegments ); // !COMMIT atomic
	ARRAY_FOREACH ( i, dToKill )
		SafeDelete ( dToKill[i] ); // unused now
}


void RtIndex_t::DumpToDisk ( const char * sFilename )
{
	CSphString sName, sError;

	CSphWriter wrHits, wrDocs, wrDict;
	sName.SetSprintf ( "%s.spp", sFilename ); wrHits.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spd", sFilename ); wrDocs.OpenFile ( sName.cstr(), sError );
	sName.SetSprintf ( "%s.spi", sFilename ); wrDict.OpenFile ( sName.cstr(), sError );

	BYTE bDummy = 1;
	wrDict.PutBytes ( &bDummy, 1 );
	wrDocs.PutBytes ( &bDummy, 1 );
	wrHits.PutBytes ( &bDummy, 1 );

	float tmStart = sphLongTimer ();

	while ( m_pSegments.GetLength()>1 )
	{
		m_pSegments.Sort ( CmpSegments_fn() );
		RtSegment_t * pSeg1 = m_pSegments.Pop();
		RtSegment_t * pSeg2 = m_pSegments.Pop();
		m_pSegments.Add ( MergeSegments ( pSeg1, pSeg2 ) );
		SafeDelete ( pSeg1 );
		SafeDelete ( pSeg2 );
	}
	RtSegment_t * pSeg = m_pSegments[0];

	float tmMerged = sphLongTimer ();
	printf ( "final merge done in %.2f sec\n", tmMerged-tmStart );

	SphWordID_t uLastWord = 0;
	SphOffset_t uLastDocpos = 0;

	static const int WORDLIST_CHECKPOINT = 1024;
	int iWords = 0;

	struct Checkpoint_t
	{
		uint64_t m_uWord;
		uint64_t m_uOffset;
	};
	CSphVector<Checkpoint_t> dCheckpoints;

	RtWordReader_t tIn ( pSeg );
	for ( ;; )
	{
		const RtWord_t * pWord = tIn.UnzipWord();
		if ( !pWord )
			break;

		SphDocID_t uLastDoc = 0;
		SphOffset_t uLastHitpos = 0;

		if ( !iWords )
		{
			Checkpoint_t & tChk = dCheckpoints.Add ();
			tChk.m_uWord = pWord->m_uWordID;
			tChk.m_uOffset = wrDict.GetPos();
		}

		wrDict.ZipInt ( pWord->m_uWordID - uLastWord );
		wrDict.ZipOffset ( wrDocs.GetPos() - uLastDocpos );
		wrDict.ZipInt ( pWord->m_uDocs );
		wrDict.ZipInt ( pWord->m_uHits );

		uLastDocpos = wrDocs.GetPos();
		uLastWord = pWord->m_uWordID;

		RtDocIterator_t tIn ( pSeg, *pWord );
		for ( ;; )
		{
			const RtDoc_t * pDoc = tIn.UnzipDoc();
			if ( !pDoc )
				break;

			wrDocs.ZipOffset ( pDoc->m_uDocID-uLastDoc );
			wrDocs.ZipOffset ( wrHits.GetPos() - uLastHitpos );
			wrDocs.ZipInt ( pDoc->m_uFields );
			wrDocs.ZipInt ( pDoc->m_uHits );
			uLastDoc = pDoc->m_uDocID;
			uLastHitpos = wrHits.GetPos();

			if ( pDoc->m_uHits>1 )
			{
				DWORD uLastHit = 0;
				for ( DWORD uHit = pDoc->m_uHit; uHit < pDoc->m_uHit + pDoc->m_uHits; uHit++ )
				{
					wrHits.ZipInt ( pSeg->m_dHits[uHit] - uLastHit );
					uLastHit = pSeg->m_dHits[uHit];
				}
			} else
			{
				wrHits.ZipInt ( pDoc->m_uHit );
			}
			wrHits.ZipInt ( 0 );
		}
		wrDocs.ZipInt ( 0 );

		if ( ++iWords==WORDLIST_CHECKPOINT )
		{
			wrDict.ZipInt ( 0 );
			wrDict.ZipOffset ( wrDocs.GetPos() - uLastDocpos ); // store last hitlist length

			uLastDocpos = 0;
			uLastWord = 0;

			iWords = 0;
		}
	}
	wrDict.ZipInt ( 0 ); // indicate checkpoint
	wrDict.ZipOffset ( wrDocs.GetPos() - uLastDocpos ); // store last doclist length

	if ( dCheckpoints.GetLength() )
		wrDict.PutBytes ( &dCheckpoints[0], dCheckpoints.GetLength()*sizeof(Checkpoint_t) );

	wrHits.CloseFile ();
	wrDocs.CloseFile ();
	wrDict.CloseFile ();

	float tmDump = sphLongTimer ();
	printf ( "dump done in %.2f sec\n", tmDump-tmMerged );
}

//////////////////////////////////////////////////////////////////////////

ISphRtIndex * sphCreateIndexRT ()
{
	return new RtIndex_t ();
}

//
// $Id$
//
