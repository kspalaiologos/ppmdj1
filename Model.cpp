/****************************************************************************
 *  This file is part of PPMd project                                       *
 *  Written and distributed to public domain by Dmitry Shkarin 1997,        *
 *  1999-2001, 2006                                                         *
 *  Contents: PPMII model description and encoding/decoding routines        *
 ****************************************************************************/
#include <string.h>
#include "PPMd.h"
#pragma hdrstop
#include "Coder.hpp"
#include "SubAlloc.hpp"

enum { UP_FREQ=5, INT_BITS=7, PERIOD_BITS=7, TOT_BITS=INT_BITS+PERIOD_BITS,
    INTERVAL=1 << INT_BITS, BIN_SCALE=1 << TOT_BITS, ROUND=16, MAX_FREQ=124,
    O_BOUND=9 };

static _THREAD1 struct SEE2_CONTEXT { // SEE-contexts for PPM-contexts with masked symbols
    WORD Summ;
    BYTE Shift, Count;
    void init(UINT InitVal) { Summ=InitVal << (Shift=PERIOD_BITS-4); Count=7; }
    UINT getMean() {
        UINT RetVal=(Summ >> Shift);        Summ -= RetVal;
        return RetVal+!RetVal;
    }
    void update() { if (--Count == 0)       setShift_rare(); }
    void setShift_rare();
} _THREAD SEE2Cont[23][32], DummySEE2Cont;
#pragma pack(1)
static _THREAD1 struct PPM_CONTEXT {
struct STATE {
    _BYTE Symbol, Freq;
    _DWORD iSuccessor;
    _PAD_TO_64(Dummy)
    PPM_CONTEXT* getSucc() const { return (PPM_CONTEXT*)Indx2Ptr(iSuccessor); }
};
    _BYTE NumStats, Flags;                  // Notes:
    _WORD SummFreq;                         // 1. NumStats & NumMasked contain
    _DWORD iStats;                          //  number of symbols minus 1
    _PAD_TO_64(Dummy1)                      // 2. contexts example:
    _DWORD iSuffix;                         //  MaxOrder:
    _PAD_TO_64(Dummy2)                      //   ABCD    context
    inline void encodeBinSymbol(int symbol);//    BCD    suffix
    inline void   encodeSymbol1(int symbol);//    BCDE   successor
    inline void   encodeSymbol2(int symbol);//  other orders:
    inline void           decodeBinSymbol();//    BCD    context
    inline void             decodeSymbol1();//     CD    suffix
    inline void             decodeSymbol2();//    BCDE   successor
    inline void           update1(STATE* p);
    inline void           update2(STATE* p);
    inline SEE2_CONTEXT*     makeEscFreq2();
    void                          rescale();
    _DWORD                cutOff(int Order);
    STATE&   oneState() const { return (STATE&) SummFreq; }
    STATE*   getStats() const { return (STATE*)Indx2Ptr(iStats); }
    PPM_CONTEXT* suff() const { return (PPM_CONTEXT*)Indx2Ptr(iSuffix); }
} * _THREAD MaxContext;
#pragma pack()

static BYTE NS2BSIndx[256], QTable[260];                // constants
static _THREAD1 PPM_CONTEXT::STATE* _THREAD FoundState; // found next state transition
static _THREAD1 int _THREAD BSumm, _THREAD OrderFall, _THREAD RunLength;
static _THREAD1 int _THREAD InitRL, _THREAD MaxOrder;
static _THREAD1 BYTE _THREAD CharMask[256], _THREAD NumMasked, _THREAD PrevSuccess;
static _THREAD1 BYTE _THREAD EscCount, _THREAD PrintCount;
static _THREAD1 WORD _THREAD BinSumm[25][64];           // binary SEE-contexts
static _THREAD1 BOOL _THREAD CutOff;

inline void SWAP(PPM_CONTEXT::STATE& s1,PPM_CONTEXT::STATE& s2) {
    _WORD t1=(_WORD&)s1;                    _DWORD t2=s1.iSuccessor;
    (_WORD&)s1 = (_WORD&)s2;                s1.iSuccessor=s2.iSuccessor;
    (_WORD&)s2 = t1;                        s2.iSuccessor=t2;
}
inline void StateCpy(PPM_CONTEXT::STATE& s1,const PPM_CONTEXT::STATE& s2) {
    (_WORD&)s1 = (_WORD&)s2;                s1.iSuccessor=s2.iSuccessor;
}
void SEE2_CONTEXT::setShift_rare()
{
    UINT i=Summ >> Shift;
    i=PERIOD_BITS-(i > 40)-(i > 280)-(i > 1020);
         if (i < Shift) { Summ >>= 1;     Shift--; }
    else if (i > Shift) { Summ <<= 1;     Shift++; }
    Count=6 << Shift;
}
static struct PPMD_STARTUP { inline PPMD_STARTUP(); } const PPMd_StartUp;
inline PPMD_STARTUP::PPMD_STARTUP()         // constants initialization
{
    UINT i, k, m, Step;
    for (i=0,k=1;i < N1     ;i++,k += 1)    Indx2Units[i]=k;
    for (k++;i < N1+N2      ;i++,k += 2)    Indx2Units[i]=k;
    for (k++;i < N1+N2+N3   ;i++,k += 3)    Indx2Units[i]=k;
    for (k++;i < N1+N2+N3+N4;i++,k += 4)    Indx2Units[i]=k;
    for (k=i=0;k < 128;k++) {
        i += (Indx2Units[i] < k+1);         Units2Indx[k]=i;
    }
    NS2BSIndx[0]=2*0;                       NS2BSIndx[1]=NS2BSIndx[2]=2*1;
    memset(NS2BSIndx+3,2*2,26);             memset(NS2BSIndx+29,2*3,256-29);
    for (i=0;i < UP_FREQ;i++)               QTable[i]=i;
    for (m=i=UP_FREQ, k=Step=1;i < 260;i++) {
        QTable[i]=m;
        if ( !--k ) { k = ++Step;           m++; }
    }
}
static void _FASTCALL StartModelRare(int MaxOrder,BOOL CutOff)
{
    int i, k, s;
    BYTE i2f[25];
    memset(CharMask,0,sizeof(CharMask));    EscCount=PrintCount=1;
    if (MaxOrder < 2) {                     // we are in solid mode
        OrderFall=::MaxOrder;
        for (PPM_CONTEXT* pc=MaxContext;pc->iSuffix != 0;pc=pc->suff())
                OrderFall--;
        return;
    }
    OrderFall=::MaxOrder=MaxOrder;          ::CutOff=CutOff;
    InitSubAllocator();
    RunLength=InitRL=-((MaxOrder < 13)?MaxOrder:13);
    MaxContext = (PPM_CONTEXT*)AllocContext();
    MaxContext->SummFreq=(MaxContext->NumStats=255)+2;
    MaxContext->iStats = Ptr2Indx(AllocUnits(256/2));
    for (PrevSuccess=i=MaxContext->iSuffix=MaxContext->Flags=0;i < 256;i++) {
        MaxContext->getStats()[i].Symbol=i; MaxContext->getStats()[i].Freq=1;
        MaxContext->getStats()[i].iSuccessor=0;
    }
    for (k=i=0;i < 25;i2f[i++]=k+1)
            while (QTable[k] == i)          k++;
static const signed char EscCoef[12]={16,-10,1,51,14,89,23,35,64,26,-42,43};
    for (k=0;k < 64;k++) {
        for (s=i=0;i < 6;i++)               s += EscCoef[2*i+((k >> i) & 1)];
        s=128*CLAMP(s,32,256-32);
        for (i=0;i < 25;i++)                BinSumm[i][k]=BIN_SCALE-s/i2f[i];
    }
    for (i=0;i < 23;i++)
            for (k=0;k < 32;k++)            SEE2Cont[i][k].init(8*i+5);
}
inline void AuxCutOff(PPM_CONTEXT::STATE* p,int Order) {
    if (Order < MaxOrder) {
        PrefetchData(p->getSucc());
        p->iSuccessor=p->getSucc()->cutOff(Order+1);
    } else                                  p->iSuccessor=0;
}
_DWORD PPM_CONTEXT::cutOff(int Order)
{
    int i, tmp, EscFreq, Scale;
    STATE* p, * p0;
    if ( !NumStats ) {
        if ((_BYTE*)(p=&oneState())->getSucc() >= UnitsStart) {
            AuxCutOff(p,Order);
            if (p->iSuccessor || Order < O_BOUND)
                    goto AT_RETURN;
        }
REMOVE: FreeUnit(this);                     return 0;
    }
    iStats = Ptr2Indx(p0=(STATE*)MoveUnitsUp(getStats(),tmp=(NumStats+2) >> 1));
    for (p=p0+(i=NumStats);p >= p0;p--)
            if ((_BYTE*)p->getSucc() < UnitsStart) {
                p->iSuccessor=0;            SWAP(*p,p0[i--]);
            } else                          AuxCutOff(p,Order);
    if (i != NumStats && Order) {
        NumStats=i;                         p=p0;
        if (i < 0) { FreeUnits(p,tmp);      goto REMOVE; }
        else if (i == 0) {
            Flags=(Flags & 0x10)+0x08*(p->Symbol >= 0x40);
            p->Freq=1+(2*(p->Freq-1))/(SummFreq-p->Freq);
            StateCpy(oneState(),*p);        FreeUnits(p,tmp);
        } else {
            iStats = Ptr2Indx(p=(STATE*)ShrinkUnits(p0,tmp,(i+2) >> 1));
            Scale=(SummFreq > 16*i);        EscFreq=SummFreq-p->Freq;
            Flags=(Flags & (0x10+0x04*Scale))+0x08*(p->Symbol >= 0x40);
            SummFreq=p->Freq=(p->Freq+Scale) >> Scale;
            do {
                EscFreq -= (++p)->Freq;
                SummFreq += (p->Freq=(p->Freq+Scale) >> Scale);
                Flags |= 0x08*(p->Symbol >= 0x40);
            } while ( --i );
            SummFreq += (EscFreq=(EscFreq+Scale) >> Scale);
        }
    }
AT_RETURN:
    if ((_BYTE*)this == UnitsStart) {
        UnitsCpy(AuxUnit,this,1);           return Ptr2Indx(AuxUnit);
    } else if((_BYTE*)suff() == UnitsStart) iSuffix=Ptr2Indx(AuxUnit);
    return Ptr2Indx(this);
}
static void RestoreModelRare(PPM_CONTEXT* pc)
{
    pText=HeapStart;
    if (!CutOff || GetUsedMemory() < (SubAllocatorSize >> 2)) {
        StartModelRare(MaxOrder,CutOff);    PrintCount=0xFF;
        EscCount=0;                         return;
    }
    for (PPM_CONTEXT::STATE* p;MaxContext->NumStats == 1 && MaxContext != pc &&
            (_BYTE*)(p=MaxContext->getStats())[1].getSucc() < UnitsStart;
            MaxContext=MaxContext->suff()) {
        MaxContext->Flags=(MaxContext->Flags & 0x10)+0x08*(p->Symbol >= 0x40);
        p->Freq=(p->Freq+1) >> 1;           StateCpy(MaxContext->oneState(),*p);
        MaxContext->NumStats=0;             FreeUnits(p,1);
    }
    while ( MaxContext->iSuffix )           MaxContext=MaxContext->suff();
    AuxUnit=UnitsStart;                     ExpandTextArea();
    do {
        PrepareTextArea();                  MaxContext->cutOff(0);
        ExpandTextArea();
    } while (GetUsedMemory() > 3*(SubAllocatorSize >> 2));
    GlueCount=GlueCount1=0;                 OrderFall=MaxOrder;
}
static _DWORD _FASTCALL CreateSuccessors(BOOL Skip,PPM_CONTEXT::STATE* p,PPM_CONTEXT* pc);
inline _DWORD ReduceOrder(PPM_CONTEXT::STATE* p,PPM_CONTEXT* pc)
{
    PPM_CONTEXT::STATE* p1;
    PPM_CONTEXT* pc1=pc;
    _DWORD iUpBranch = FoundState->iSuccessor = Ptr2Indx(pText);
    BYTE tmp, sym=FoundState->Symbol;       OrderFall++;
    if ( p ) { pc=pc->suff();               goto LOOP_ENTRY; }
    for ( ; ; ) {
        if ( !pc->iSuffix )                 return Ptr2Indx(pc);
        pc=pc->suff();
        if ( pc->NumStats ) {
            if ((p=pc->getStats())->Symbol != sym)
                    do { tmp=p[1].Symbol;   p++; } while (tmp != sym);
            tmp=2*(p->Freq < MAX_FREQ-3);
            p->Freq += tmp;                 pc->SummFreq += tmp;
        } else { p=&(pc->oneState());       p->Freq += (p->Freq < 11); }
LOOP_ENTRY:
        if ( p->iSuccessor )                break;
        p->iSuccessor=iUpBranch;            OrderFall++;
    }
    if (p->iSuccessor <= iUpBranch) {
        p1=FoundState;                      FoundState=p;
        p->iSuccessor=CreateSuccessors(FALSE,NULL,pc);
        FoundState=p1;
    }
    if (OrderFall == 1 && pc1 == MaxContext) {
        FoundState->iSuccessor=p->iSuccessor;
        pText--;
    }
    return p->iSuccessor;
}
void PPM_CONTEXT::rescale()
{
    UINT f0, sf, EscFreq, a=(OrderFall != 0), i=NumStats;
    STATE tmp, * p1, * p;                   Flags &= 0x14;
    for (p=FoundState;p != getStats();p--)  SWAP(p[0],p[-1]);
    f0=p->Freq;                             sf=SummFreq;
    EscFreq=SummFreq-p->Freq;               SummFreq=p->Freq=(p->Freq+a) >> 1;
    do {
        EscFreq -= (++p)->Freq;
        SummFreq += (p->Freq=(p->Freq+a) >> 1);
        if ( p->Freq )                      Flags |= 0x08*(p->Symbol >= 0x40);
        if (p[0].Freq > p[-1].Freq) {
            StateCpy(tmp,*(p1=p));
            do { StateCpy(p1[0],p1[-1]); } while (tmp.Freq > (--p1)[-1].Freq);
            StateCpy(*p1,tmp);
        }
    } while ( --i );
    if (p->Freq == 0) {
        do { i++; } while ((--p)->Freq == 0);
        EscFreq += i;                       a=(NumStats+2) >> 1;
        if ((NumStats -= i) == 0) {
            StateCpy(tmp,*getStats());      Flags &= 0x18;
            tmp.Freq=(2*tmp.Freq+EscFreq-1)/EscFreq;
            if (tmp.Freq > MAX_FREQ/3)      tmp.Freq=MAX_FREQ/3;
            FreeUnits(getStats(),a);        StateCpy(oneState(),tmp);
            FoundState=&oneState();         return;
        }
        iStats = Ptr2Indx(ShrinkUnits(getStats(),a,(NumStats+2) >> 1));
    }
    SummFreq += (EscFreq+1) >> 1;
    if (OrderFall || (Flags & 0x04) == 0) {
        a=(sf -= EscFreq)-f0;
        a=CLAMP(UINT((f0*SummFreq-sf*getStats()->Freq+a-1)/a),2U,MAX_FREQ/2U-18U);
    } else                                  a=2;
    (FoundState=getStats())->Freq += a;     SummFreq += a;
    Flags |= 0x04;
}
static _DWORD _FASTCALL CreateSuccessors(BOOL Skip,PPM_CONTEXT::STATE* p,PPM_CONTEXT* pc)
{
    PPM_CONTEXT ct;
    _DWORD iUpBranch = FoundState->iSuccessor;
    PPM_CONTEXT::STATE* ps[MAX_O], ** pps=ps;
    UINT cf, s0;
    BYTE tmp, sym=FoundState->Symbol;
    if ( !Skip ) {
        *pps++ = FoundState;
        if ( !pc->iSuffix )                 goto NO_LOOP;
    }
    if ( p ) { pc=pc->suff();               goto LOOP_ENTRY; }
    do {
        pc=pc->suff();
        if ( pc->NumStats ) {
            if ((p=pc->getStats())->Symbol != sym)
                    do { tmp=p[1].Symbol;   p++; } while (tmp != sym);
            tmp=(p->Freq < MAX_FREQ);
            p->Freq += tmp;                 pc->SummFreq += tmp;
        } else {
            p=&(pc->oneState());
            p->Freq += (!pc->suff()->NumStats & (p->Freq < 11));
        }
LOOP_ENTRY:
        if (p->iSuccessor != iUpBranch) {
            pc=p->getSucc();                break;
        }
        *pps++ = p;
    } while ( pc->iSuffix );
NO_LOOP:
    if (pps == ps)                          return Ptr2Indx(pc);
    ct.NumStats=0;                          ct.Flags=0x10*(sym >= 0x40);
    ct.oneState().Symbol=sym=*(_BYTE*)Indx2Ptr(iUpBranch);
    ct.oneState().iSuccessor=Ptr2Indx((_BYTE*)Indx2Ptr(iUpBranch)+1);
    ct.Flags |= 0x08*(sym >= 0x40);
    if ( pc->NumStats ) {
        if ((p=pc->getStats())->Symbol != sym)
                do { tmp=p[1].Symbol;       p++; } while (tmp != sym);
        s0=pc->SummFreq-pc->NumStats-(cf=p->Freq-1);
        cf=1+((2*cf <= s0)?(12*cf > s0):((cf+2*s0)/s0));
        ct.oneState().Freq=(cf < 7)?(cf):(7);
    } else
            ct.oneState().Freq=pc->oneState().Freq;
    do {
        PPM_CONTEXT* pc1 = (PPM_CONTEXT*)AllocContext();
        if ( !pc1 )                         return 0;
        ((DWORD*)pc1)[0]=((DWORD*)&ct)[0];  ((DWORD*)pc1)[1]=((DWORD*)&ct)[1];
#if defined(_32_EXOTIC) || defined(_64_EXOTIC)
        ((DWORD*)pc1)[2]=((DWORD*)&ct)[2];  ((DWORD*)pc1)[3]=((DWORD*)&ct)[3];
#endif /* defined(_32_EXOTIC) || defined(_64_EXOTIC) */
        pc1->iSuffix=Ptr2Indx(pc);
        (*--pps)->iSuccessor=Ptr2Indx(pc=pc1);
    } while (pps != ps);
    return Ptr2Indx(pc);
}
// Tabulated escapes for exponential symbol distribution
static const BYTE ExpEscape[16]={51,43,18,12,11,9,8,7,6,5,4,3,3,2,2,2};
static void _FASTCALL UpdateModel(PPM_CONTEXT* MinContext)
{
    BYTE Flag, sym, FSymbol=FoundState->Symbol;
    UINT ns1, ns, cf, sf, s0, FFreq=FoundState->Freq;
    _DWORD iSuccessor, iFSuccessor=FoundState->iSuccessor;
    PPM_CONTEXT* pc;
    PPM_CONTEXT::STATE* p=NULL;
    if ( MinContext->iSuffix ) {
        pc=MinContext->suff();
        if ( pc->NumStats ) {
            if ((p=pc->getStats())->Symbol != FSymbol) {
                do { sym=p[1].Symbol;       p++; } while (sym != FSymbol);
                if (p[0].Freq >= p[-1].Freq) {
                    SWAP(p[0],p[-1]);       p--;
                }
            }
            if (p->Freq < MAX_FREQ) {
                cf=1+(FFreq < 4*8);
                p->Freq += cf;              pc->SummFreq += cf;
            }
        } else { p=&(pc->oneState());       p->Freq += (p->Freq < 11); }
    }
    pc=MaxContext;
    if (!OrderFall && iFSuccessor) {
        FoundState->iSuccessor=CreateSuccessors(TRUE,p,MinContext);
        if ( !FoundState->iSuccessor )      goto RESTART_MODEL;
        MaxContext=FoundState->getSucc();   return;
    }
    *pText++ = FSymbol;                     iSuccessor = Ptr2Indx(pText);
    if (pText >= UnitsStart)                goto RESTART_MODEL;
    if ( iFSuccessor ) {
        if ((_BYTE*)Indx2Ptr(iFSuccessor) < UnitsStart)
                iFSuccessor=CreateSuccessors(FALSE,p,MinContext);
        else                                PrefetchData(Indx2Ptr(iFSuccessor));
    } else
                iFSuccessor=ReduceOrder(p,MinContext);
    if ( !iFSuccessor )                     goto RESTART_MODEL;
    if ( !--OrderFall ) {
        iSuccessor=iFSuccessor;             pText -= (MaxContext != MinContext);
    }
    s0=MinContext->SummFreq-FFreq;          ns=MinContext->NumStats;
    Flag=0x08*(FSymbol >= 0x40);
    for ( ;pc != MinContext;pc=pc->suff()) {
        if ((ns1=pc->NumStats) != 0) {
            if ((ns1 & 1) != 0) {
                p=(PPM_CONTEXT::STATE*)ExpandUnits(pc->getStats(),(ns1+1) >> 1);
                if ( !p )                   goto RESTART_MODEL;
                pc->iStats=Ptr2Indx(p);
            }
            pc->SummFreq += QTable[ns+4] >> 3;
        } else {
            p=(PPM_CONTEXT::STATE*)AllocUnits(1);
            if ( !p )                       goto RESTART_MODEL;
            StateCpy(*p,pc->oneState());    pc->iStats=Ptr2Indx(p);
            p->Freq=(p->Freq <= MAX_FREQ/3)?(2*p->Freq-1):(MAX_FREQ-15);
            pc->SummFreq=p->Freq+(ns > 1)+ExpEscape[QTable[BSumm >> 8]];
        }
        cf=2*FFreq*(pc->SummFreq+4);        sf=s0+pc->SummFreq;
        if (cf <= 6*sf) {
            cf=1+(cf > sf)+(cf > 3*sf);     pc->SummFreq += 4;
        } else
                pc->SummFreq += (cf=4+(cf > 8*sf)+(cf > 10*sf)+(cf > 13*sf));
        p=pc->getStats()+(++pc->NumStats);  p->iSuccessor=iSuccessor;
        p->Symbol = FSymbol;                p->Freq = cf;
        pc->Flags |= Flag;
    }
    MaxContext = (PPM_CONTEXT*)Indx2Ptr(iFSuccessor);
    return;
RESTART_MODEL:
    RestoreModelRare(pc);
}
#define GET_MEAN(SUMM,SHIFT) ((SUMM+ROUND) >> SHIFT)
inline void PPM_CONTEXT::encodeBinSymbol(int symbol)
{
    STATE& rs=oneState();
    WORD& bs=BinSumm[QTable[rs.Freq-1]][NS2BSIndx[suff()->NumStats]+PrevSuccess+
            Flags+((RunLength >> 26) & 0x20)];
    UINT tmp=rcBinStart(BSumm=bs,TOT_BITS); bs -= GET_MEAN(BSumm,PERIOD_BITS);
    if (rs.Symbol == symbol) {
        bs += INTERVAL;                     rcBinCorrect0(tmp);
        FoundState=&rs;                     rs.Freq += (rs.Freq < 196);
        RunLength++;                        PrevSuccess=1;
    } else {
        rcBinCorrect1(tmp,BIN_SCALE-BSumm); CharMask[rs.Symbol]=EscCount;
        NumMasked=PrevSuccess=0;            FoundState=NULL;
    }
}
inline void PPM_CONTEXT::decodeBinSymbol()
{
    STATE& rs=oneState();
    WORD& bs=BinSumm[QTable[rs.Freq-1]][NS2BSIndx[suff()->NumStats]+PrevSuccess+
            Flags+((RunLength >> 26) & 0x20)];
    UINT tmp=rcBinStart(BSumm=bs,TOT_BITS); bs -= GET_MEAN(BSumm,PERIOD_BITS);
    if ( !rcBinDecode(tmp) ) {
        bs += INTERVAL;                     rcBinCorrect0(tmp);
        FoundState=&rs;                     rs.Freq += (rs.Freq < 196);
        RunLength++;                        PrevSuccess=1;
    } else {
        rcBinCorrect1(tmp,BIN_SCALE-BSumm); CharMask[rs.Symbol]=EscCount;
        NumMasked=PrevSuccess=0;            FoundState=NULL;
    }
}
inline void PPM_CONTEXT::update1(STATE* p)
{
    (FoundState=p)->Freq += 4;              SummFreq += 4;
    if (p[0].Freq > p[-1].Freq) {
        SWAP(p[0],p[-1]);                   FoundState=--p;
        if (p->Freq > MAX_FREQ)             rescale();
    }
}
inline void PPM_CONTEXT::encodeSymbol1(int symbol)
{
    STATE* p=getStats();
    UINT i=p->Symbol, LoCnt=p->Freq;        Range.scale=SummFreq;
    if (i == symbol) {
        PrevSuccess=(2*(Range.high=LoCnt) > Range.scale);
        (FoundState=p)->Freq=(LoCnt += 4);  SummFreq += 4;
        if (LoCnt > MAX_FREQ)               rescale();
        Range.low=0;                        return;
    }
    PrefetchData(p+(i=NumStats));           PrevSuccess=0;
    while ((++p)->Symbol != symbol) {
        LoCnt += p->Freq;
        if (--i == 0) {
            if ( iSuffix )                  PrefetchData(suff());
            Range.low=LoCnt;                CharMask[p->Symbol]=EscCount;
            i=NumMasked=NumStats;           FoundState=NULL;
            do { CharMask[(--p)->Symbol]=EscCount; } while ( --i );
            Range.high=Range.scale;         return;
        }
    }
    Range.high=(Range.low=LoCnt)+p->Freq;   update1(p);
}
inline void PPM_CONTEXT::decodeSymbol1()
{
    STATE* p=getStats();
    UINT i, count, HiCnt=p->Freq;           Range.scale=SummFreq;
    if ((count=rcGetCurrentCount()) < HiCnt) {
        PrevSuccess=(2*(Range.high=HiCnt) > Range.scale);
        (FoundState=p)->Freq=(HiCnt += 4);  SummFreq += 4;
        if (HiCnt > MAX_FREQ)               rescale();
        Range.low=0;                        return;
    }
    PrefetchData(p+(i=NumStats));           PrevSuccess=0;
    while ((HiCnt += (++p)->Freq) <= count)
        if (--i == 0) {
            if ( iSuffix )                  PrefetchData(suff());
            Range.low=HiCnt;                CharMask[p->Symbol]=EscCount;
            i=NumMasked=NumStats;           FoundState=NULL;
            do { CharMask[(--p)->Symbol]=EscCount; } while ( --i );
            Range.high=Range.scale;         return;
        }
    Range.low=(Range.high=HiCnt)-p->Freq;   update1(p);
}
inline void PPM_CONTEXT::update2(STATE* p)
{
    (FoundState=p)->Freq += 4;              SummFreq += 4;
    if (p->Freq > MAX_FREQ)                 rescale();
    EscCount++;                             RunLength=InitRL;
}
inline SEE2_CONTEXT* PPM_CONTEXT::makeEscFreq2()
{
    SEE2_CONTEXT* psee2c;                   PrefetchData(getStats());
    if (NumStats != 0xFF) {
        psee2c=SEE2Cont[QTable[NumStats+3]-4]+(SummFreq > 10*(NumStats+1))+
                2*(2*NumStats < suff()->NumStats+NumMasked)+Flags;
        Range.scale=psee2c->getMean();
    } else { psee2c=&DummySEE2Cont;         Range.scale=1; }
    PrefetchData(getStats()+NumStats);      return psee2c;
}
inline void PPM_CONTEXT::encodeSymbol2(int symbol)
{
    SEE2_CONTEXT* psee2c=makeEscFreq2();
    UINT Sym, LoCnt=0, i=NumStats-NumMasked;
    STATE* p1, * p=getStats()-1;
    do {
        do { Sym=p[1].Symbol;   p++; } while (CharMask[Sym] == EscCount);
        CharMask[Sym]=EscCount;
        if (Sym == symbol)                  goto SYMBOL_FOUND;
        LoCnt += p->Freq;
    } while ( --i );
    Range.high=(Range.scale += (Range.low=LoCnt));
    psee2c->Summ += Range.scale;            NumMasked = NumStats;
    return;
SYMBOL_FOUND:
    Range.low=LoCnt;                        Range.high=(LoCnt += p->Freq);
    for (p1=p; --i ; ) {
        do { Sym=p1[1].Symbol;  p1++; } while (CharMask[Sym] == EscCount);
        LoCnt += p1->Freq;
    }
    Range.scale += LoCnt;
    psee2c->update();                       update2(p);
}
inline void PPM_CONTEXT::decodeSymbol2()
{
    SEE2_CONTEXT* psee2c=makeEscFreq2();
    UINT Sym, count, HiCnt=0, i=NumStats-NumMasked;
    STATE* ps[256], ** pps=ps, * p=getStats()-1;
    do {
        do { Sym=p[1].Symbol;   p++; } while (CharMask[Sym] == EscCount);
        HiCnt += p->Freq;                   *pps++ = p;
    } while ( --i );
    Range.scale += HiCnt;                   count=rcGetCurrentCount();
    p=*(pps=ps);
    if (count < HiCnt) {
        HiCnt=0;
        while ((HiCnt += p->Freq) <= count) p=*++pps;
        Range.low = (Range.high=HiCnt)-p->Freq;
        psee2c->update();                   update2(p);
    } else {
        Range.low=HiCnt;                    Range.high=Range.scale;
        i=NumStats-NumMasked;               NumMasked = NumStats;
        do { CharMask[(*pps)->Symbol]=EscCount; pps++; } while ( --i );
        psee2c->Summ += Range.scale;
    }
}
inline void ClearMask(_PPMD_FILE* EncodedFile,_PPMD_FILE* DecodedFile)
{
    EscCount=1;                             memset(CharMask,0,sizeof(CharMask));
    if (++PrintCount == 0)                  PrintInfo(DecodedFile,EncodedFile);
}
void _STDCALL EncodeFile(_PPMD_FILE* EncodedFile,_PPMD_FILE* DecodedFile,
                            int MaxOrder,BOOL CutOff)
{
    rcInitEncoder();                        StartModelRare(MaxOrder,CutOff);
    for (PPM_CONTEXT* MinContext=MaxContext; ; ) {
        int c = _PPMD_E_GETC(DecodedFile);
        if ( MinContext->NumStats ) {
            MinContext->encodeSymbol1(c);   rcEncodeSymbol();
        } else                              MinContext->encodeBinSymbol(c);
        while ( !FoundState ) {
            RC_ENC_NORMALIZE(EncodedFile);
            do {
                if ( !MinContext->iSuffix ) goto STOP_ENCODING;
                OrderFall++;                MinContext=MinContext->suff();
            } while (MinContext->NumStats == NumMasked);
            MinContext->encodeSymbol2(c);   rcEncodeSymbol();
        }
        if (!OrderFall && (_BYTE*)FoundState->getSucc() >= UnitsStart)
                PrefetchData(MaxContext=FoundState->getSucc());
        else {
            UpdateModel(MinContext);
            if (EscCount == 0)              ClearMask(EncodedFile,DecodedFile);
        }
        RC_ENC_NORMALIZE(EncodedFile);      MinContext=MaxContext;
    }
STOP_ENCODING:
    rcFlushEncoder(EncodedFile);            PrintInfo(DecodedFile,EncodedFile);
}
void _STDCALL DecodeFile(_PPMD_FILE* DecodedFile,_PPMD_FILE* EncodedFile,
                            int MaxOrder,BOOL CutOff)
{
    rcInitDecoder(EncodedFile);             StartModelRare(MaxOrder,CutOff);
    for (PPM_CONTEXT* MinContext=MaxContext; ; ) {
        if ( MinContext->NumStats ) {
            MinContext->decodeSymbol1();    rcRemoveSubrange();
        } else                              MinContext->decodeBinSymbol();
        while ( !FoundState ) {
            RC_DEC_NORMALIZE(EncodedFile);
            do {
                if ( !MinContext->iSuffix ) goto STOP_DECODING;
                OrderFall++;                MinContext=MinContext->suff();
            } while (MinContext->NumStats == NumMasked);
            MinContext->decodeSymbol2();    rcRemoveSubrange();
        }
        _PPMD_D_PUTC(FoundState->Symbol,DecodedFile);
        if (!OrderFall && (_BYTE*)FoundState->getSucc() >= UnitsStart)
                PrefetchData(MaxContext=FoundState->getSucc());
        else {
            UpdateModel(MinContext);
            if (EscCount == 0)              ClearMask(EncodedFile,DecodedFile);
        }
        RC_DEC_NORMALIZE(EncodedFile);      MinContext=MaxContext;
    }
STOP_DECODING:
    PrintInfo(DecodedFile,EncodedFile);
}
#if !defined(NDEBUG)
#pragma option -w-ccc -w-rch
BOOL TestCompilation()
{
/*****************  testing on proper sizes of data types  ******************/
    if (2*sizeof(_BYTE) != sizeof(_WORD) || 12*sizeof(_BYTE) != sizeof(PPM_CONTEXT))
            return FALSE;
/*************  testing on proper alignment of our data types  **************/
    PPM_CONTEXT tmp;
    if (2*sizeof(PPM_CONTEXT::STATE) != sizeof(PPM_CONTEXT) || &tmp.iSuffix != (_DWORD*)(&tmp.oneState()+1))
            return FALSE;
    return TRUE;
}
#endif /* !defined(NDEBUG) */
