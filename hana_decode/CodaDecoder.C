////////////////////////////////////////////////////////////////////
//
//   CodaDecoder
//
//    Object Oriented version of decoder
//    Sept, 2014    R. Michaels
//
/////////////////////////////////////////////////////////////////////

#include "CodaDecoder.h"
#include "THaCrateMap.h"
#include "THaBenchmark.h"
#include "TError.h"
#include <iostream>

using namespace std;

namespace Decoder {

//   static const Int_t MAX_EVTYPES = 200;
//   static const Int_t MAX_PHYS_EVTYPES = 14;

//_____________________________________________________________________________
CodaDecoder::CodaDecoder()
{
  irn = new Int_t[MAXROC];
  fbfound = new Int_t[MAXROC*MAXSLOT];
  memset(irn, 0, MAXROC*sizeof(Int_t));
  memset(fbfound, 0, MAXROC*MAXSLOT*sizeof(Int_t));
  fDebugFile = 0;
  fDebug=0;
  fNeedInit=true;
  first_decode=kFALSE;
  fMultiBlockMode=kFALSE;
  fBlockIsDone=kFALSE;
}

//_____________________________________________________________________________
CodaDecoder::~CodaDecoder()
{
  delete [] irn;
  delete [] fbfound;
}

//_____________________________________________________________________________
Int_t CodaDecoder::GetPrescaleFactor(Int_t trigger_type) const
{
  // To get the prescale factors for trigger number "trigger_type"
  // (valid types are 1,2,3...)
  //  if (fgPsFact) return fgPsFact->GetPrescaleFactor(trigger_type);
  return 0;
}

//_____________________________________________________________________________
Int_t CodaDecoder::Init() {
  Int_t ret = HED_OK;
  ret = init_cmap();
  if (fMap) fMap->print();
  if (ret != HED_OK) return ret;
  ret = init_slotdata(fMap);
  first_decode = kFALSE;
  fNeedInit = kFALSE;
  return ret;
}

//_____________________________________________________________________________
Int_t CodaDecoder::LoadEvent(const UInt_t* evbuffer)
{
  // Main engine for decoding, called by public LoadEvent() methods
 static Int_t fdfirst=1;
 static Int_t chkfbstat=1;
 if (fDebugFile) *fDebugFile << "CodaDecode:: Loading event  ... "<<endl;
 if (fDebugFile) *fDebugFile << "evbuffer ptr "<<evbuffer<<endl;
  assert( evbuffer );
  assert( fMap || fNeedInit );
  Int_t ret = HED_OK;
  buffer = evbuffer;
  if(fDebugFile) {
    *fDebugFile << "CodaDecode:: dumping "<<endl;
    dump(evbuffer);
  }
  if (first_decode || fNeedInit) {
    ret = init_cmap();
    if (fDebugFile) {
	 *fDebugFile << "\n CodaDecode:: Print of Crate Map"<<endl;
	 fMap->print(fDebugFile);
    } else {
      fMap->print();
    }
    if( ret != HED_OK ) return ret;
    ret = init_slotdata(fMap);
    if( ret != HED_OK ) return ret;
    FindUsedSlots();
    first_decode=kFALSE;
  }
  if( fDoBench ) fBench->Begin("clearEvent");
  for( Int_t i=0; i<fNSlotClear; i++ ) crateslot[fSlotClear[i]]->clearEvent();
  if( fDoBench ) fBench->Stop("clearEvent");
  event_length = evbuffer[0]+1;  // in longwords (4 bytes)
  event_type = evbuffer[1]>>16;
  if(event_type < 0) return HED_ERR;
  event_num = 0;
  if (event_type == PRESTART_EVTYPE) {
     // Usually prestart is the first 'event'.  Call SetRunTime() to
     // re-initialize the crate map since we now know the run time.
     // This won't happen for split files (no prestart). For such files,
     // the user should call SetRunTime() explicitly.
     SetRunTime(static_cast<ULong64_t>(evbuffer[2]));
     run_num  = evbuffer[3];
     run_type = evbuffer[4];
     evt_time = fRunTime;
  }
  if (event_type <= MAX_PHYS_EVTYPE) {
     event_num = evbuffer[4];
     recent_event = event_num;
     FindRocs(evbuffer);
     if ((fdfirst==1) & (fDebugFile!=0)) {
       fdfirst=0;
       CompareRocs();
     }

   // Decode each ROC
   // This is not part of the loop above because it may exit prematurely due
   // to errors, which would leave the rocdat[] array incomplete.

    for( Int_t i=0; i<nroc; i++ ) {

      Int_t iroc = irn[i];
      const RocDat_t* proc = rocdat+iroc;
      Int_t ipt = proc->pos + 1;
      Int_t iptmax = proc->pos + proc->len;

      if (fMap->isFastBus(iroc)) {  // checking that slots found = expected
	  if (GetEvNum() > 200 && chkfbstat < 3) chkfbstat=2;
	  if (chkfbstat == 1) ChkFbSlot(iroc, evbuffer, ipt, iptmax);
	  if (chkfbstat == 2) {
	      ChkFbSlots();
	      chkfbstat = 3;
	  }
      }

      Int_t status;

 // If at least one module is in a bank, must split the banks for this roc

      if (fMap->isBankStructure(iroc)) {
	  if (fDebugFile) *fDebugFile << "\nCodaDecode::Calling bank_decode "<<i<<"   "<<iroc<<"  "<<ipt<<"  "<<iptmax<<endl;
	  /*status =*/ bank_decode(iroc,evbuffer,ipt,iptmax);
      }

      if (fDebugFile) *fDebugFile << "\nCodaDecode::Calling roc_decode "<<i<<"   "<<evbuffer<<"  "<<iroc<<"  "<<ipt<<"  "<<iptmax<<endl;

      status = roc_decode(iroc,evbuffer, ipt, iptmax);

      // do something with status
      if (status == -1) break;

    }
  }

  return ret;
}

//_____________________________________________________________________________
Int_t CodaDecoder::LoadFromMultiBlock()
{
  // LoadFromMultiBlock : This assumes the data are in multiblock mode.

  // For modules that are in multiblock mode, the next event is loaded.
  // For other modules not in multiblock mode (e.g. scalers) or other data (e.g. flags)
  // the data remain "stale" until the next block of events.

  if (!fMultiBlockMode) return HED_ERR;
  fBlockIsDone = kFALSE;

  for( Int_t i=0; i<fNSlotClear; i++ ) {
    if (crateslot[fSlotClear[i]]->GetModule()->IsMultiBlockMode()) crateslot[fSlotClear[i]]->clearEvent();
  }

  for( Int_t i=0; i<nroc; i++ ) {

      Int_t roc = irn[i];
      Int_t minslot = fMap->getMinSlot(roc);
      Int_t maxslot = fMap->getMaxSlot(roc);
      for (Int_t slot = minslot; slot <= maxslot; slot++) {
	if (fMap->slotUsed(roc,slot) && crateslot[idx(roc,slot)]->GetModule()->IsMultiBlockMode()) {
	  crateslot[idx(roc,slot)]->LoadNextEvBuffer();
	  if (crateslot[idx(roc,slot)]->BlockIsDone()) fBlockIsDone = kTRUE;
	}
      }
  }
  return HED_OK;
}


//_____________________________________________________________________________
Int_t CodaDecoder::roc_decode( Int_t roc, const UInt_t* evbuffer,
				  Int_t ipt, Int_t istop )
{
  // Decode a Readout controller
  assert( evbuffer && fMap );
  if( fDoBench ) fBench->Begin("roc_decode");
  Int_t slot;
  Int_t Nslot = fMap->getNslot(roc);
  Int_t minslot = fMap->getMinSlot(roc);
  Int_t maxslot = fMap->getMaxSlot(roc);
  Int_t retval = HED_OK;
  Int_t nwords;
  synchmiss = false;
  synchextra = false;
  buffmode = false;
  const UInt_t* p      = evbuffer+ipt;    // Points to ROC ID word (1 before data)
  const UInt_t* pstop  =evbuffer+istop;   // Points to last word of data
  fBlockIsDone = kFALSE;

  Int_t firstslot, incrslot;
  Int_t n_slots_checked, n_slots_done;

  Bool_t slotdone;

  Int_t status = SD_ERR;

  n_slots_done = 0;

  if (istop >= event_length) {
    cerr << "ERROR:: roc_decode:  stop point exceeds event length (?!)"<<endl;
    goto err;
  }

  if (fMap->isFastBus(roc)) {  // higher slot # appears first in multiblock mode
      firstslot=maxslot;       // the decoding order improves efficiency
      incrslot = -1;
  } else {
      firstslot=minslot;
      incrslot = 1;
  }

  if (fDebugFile) {
    *fDebugFile << "CodaDecode:: roc_decode:: roc#  "<<dec<<roc<<" nslot "<<Nslot<<endl;
    *fDebugFile << "CodaDecode:: roc_decode:: firstslot "<<dec<<firstslot<<"  incrslot "<<incrslot<<endl;
  }

  if (Nslot <= 0) goto err;
  fMap->setSlotDone();      // clears the "done" bits

  while ( p++ < pstop && n_slots_done < Nslot ) {

    if (fDebugFile) {
	 *fDebugFile << "CodaDecode::roc_decode:: evbuff "<<(p-evbuffer)<<"  "<<hex<<*p<<dec<<endl;
	 *fDebugFile << "CodaDecode::roc_decode:: n_slots_done "<<n_slots_done<<"  "<<firstslot<<endl;
    }

    LoadIfFlagData(p);

    n_slots_checked = 0;
    slot = firstslot;
    slotdone = kFALSE;
// bank structure is decoded with bank_decode
    if (fMap->getBank(roc,slot) >= 0) {
      n_slots_done++;
      slotdone=kTRUE;
    }

    while(!slotdone && n_slots_checked < Nslot-n_slots_done && slot >= 0 && slot < MAXSLOT) {


      if (!fMap->slotUsed(roc,slot) || fMap->slotDone(slot)) {
	 slot = slot + incrslot;
	 continue;
      }

      ++n_slots_checked;

     if (fDebugFile) {
       *fDebugFile<< "roc_decode:: slot logic "<<roc<<"  "<<slot<<"  "<<firstslot<<"  "<<n_slots_checked<<"  "<<Nslot-n_slots_done<<endl;
     }

      nwords = crateslot[idx(roc,slot)]->LoadIfSlot(p, pstop);

      if (nwords > 0) {
	   p = p + nwords - 1;
	   fMap->setSlotDone(slot);
	   n_slots_done++;
	   if(fDebugFile) *fDebugFile << "CodaDecode::  slot "<<slot<<"  is DONE    "<<nwords<<endl;
	   slotdone = kTRUE;
      }

      if (crateslot[idx(roc,slot)]->IsMultiBlockMode()) fMultiBlockMode = kTRUE;
      if (crateslot[idx(roc,slot)]->BlockIsDone()) fBlockIsDone = kTRUE;

      if (fDebugFile) {
	  *fDebugFile<< "CodaDecode:: roc_decode:: after LoadIfSlot "<<p << "  "<<pstop<<"  "<<"  "<<hex<<*p<<"  "<<dec<<nwords<<endl;
      }

      slot = slot + incrslot;

    }

  } //end while(p++<pstop)
  goto exit;

 err:
  retval = (status == SD_ERR) ? HED_ERR : HED_WARN;
 exit:
  if( fDoBench ) fBench->Stop("roc_decode");
  return retval;
}

//_____________________________________________________________________________
Int_t CodaDecoder::bank_decode( Int_t roc, const UInt_t* evbuffer,
				  Int_t ipt, Int_t istop )
{
  // Split a roc into banks, if using bank structure
  // Then loop over slots and decode it from a bank if the slot
  // belongs to a bank.
  assert( evbuffer && fMap );
  if( fDoBench ) fBench->Begin("bank_decode");
  Int_t retval = HED_OK;
  if (!fMap->isBankStructure(roc)) return retval;
  fBlockIsDone = kFALSE;

  Int_t pos,len,bank,head;

  memset(bankdat,0,MAXBANK*sizeof(BankDat_t));

  if (fDebugFile) *fDebugFile << "CodaDecode:: bank_decode  ... "<<roc<<"   "<<ipt<<"  "<<istop<<endl;

  pos = ipt+1;  // ipt points to ROC ID word

  while (pos < istop) {
    len = evbuffer[pos];
    head = evbuffer[pos+1];
    bank = (head>>16)&0xffff;
    if (fDebugFile) *fDebugFile << "bank 0x"<<hex<<bank<<"  head 0x"<<head<<"    len 0x"<<len<<dec<<endl;

    if (bank >= 0 && bank < MAXBANK) {
      bankdat[bank].pos=pos+2;
      bankdat[bank].len=len-1;
    }

    pos += len+1;

  }

  Int_t minslot = fMap->getMinSlot(roc);
  Int_t maxslot = fMap->getMaxSlot(roc);

  for (Int_t slot = minslot; slot <= maxslot; slot++) {
    if (!fMap->slotUsed(roc,slot)) continue;
    bank=fMap->getBank(roc,slot);
    if (bank < 0 || bank >= Decoder::MAXBANK) {
      cerr << "CodaDecoder::ERROR:  bank number out of range "<<endl;
      return 0;
    }
    pos = bankdat[bank].pos;
    len = bankdat[bank].len;
    if (fDebugFile) *fDebugFile << "CodaDecode:: loading bank "<<roc<<"  "<<slot<<"   "<<bank<<"  "<<pos<<"   "<<len<<endl;
    crateslot[idx(roc,slot)]->LoadBank(evbuffer,pos,len);
    if (crateslot[idx(roc,slot)]->IsMultiBlockMode()) fMultiBlockMode = kTRUE;
    if (crateslot[idx(roc,slot)]->BlockIsDone()) fBlockIsDone = kTRUE;
  }

  if( fDoBench ) fBench->Stop("bank_decode");
  return retval;
}


//_____________________________________________________________________________
Int_t CodaDecoder::LoadIfFlagData(const UInt_t* evbuffer)
{
  // Need to generalize this ...  too Hall A specific
  //
  // Looks for buffer mode and synch problems.  The latter are recoverable
  // but extremely rare, so I haven't bothered to write recovery a code yet,
  // but at least this warns you.
  assert( evbuffer );
  UInt_t word   = *evbuffer;
  UInt_t upword = word & 0xffff0000;
  if (fDebugFile) *fDebugFile << "CodaDecode:: TestBit on :  Flag data ? "<<hex<<word<<dec<<endl;
  if( word == 0xdc0000ff) synchmiss = true;
  if( upword == 0xdcfe0000) {
    synchextra = true;
    Int_t slot = (word&0xf800)>>11;
    Int_t nhit = (word&0x7ff);
    if(fDebug>0) {
      cout << "THaEvData: WARNING: Fastbus slot ";
      cout << slot << "  has extra hits "<<nhit<<endl;
    }
  }
  if( upword == 0xfabc0000) {
    datascan = *(evbuffer+3);
    if(fDebug>0 && (synchmiss || synchextra)) {
      cout << "THaEvData: WARNING: Synch problems !"<<endl;
      cout << "Data scan word 0x"<<hex<<datascan<<dec<<endl;
    }
  }
  if( upword == 0xfabb0000) buffmode = false;
  if((word&0xffffff00) == 0xfafbbf00) {
    buffmode = true;
    synchflag = word&0xff;
  }
  return HED_OK;
}



Int_t CodaDecoder::FindRocs(const UInt_t *evbuffer) {

  assert( evbuffer && fMap );
#ifdef FIXME
  if( fDoBench ) fBench->Begin("physics_decode");
#endif
  Int_t status = HED_OK;

  if( (evbuffer[1]&0xffff) != 0x10cc ) std::cout<<"Warning, header error"<<std::endl;
  if( event_type > MAX_PHYS_EVTYPE ) std::cout<<"Warning, Event type makes no sense"<<std::endl;
  memset(rocdat,0,MAXROC*sizeof(RocDat_t));
  // Set pos to start of first ROC data bank
  Int_t pos = evbuffer[2]+3;  // should be 7
  nroc = 0;
  while( pos+1 < event_length && nroc < MAXROC ) {
    Int_t len  = evbuffer[pos];
    Int_t iroc = (evbuffer[pos+1]&0xff0000)>>16;
    if( iroc>=MAXROC ) {
#ifdef FIXME
      if(fDebug>0) {
	cout << "ERROR in EvtTypeHandler::FindRocs "<<endl;
	cout << "  illegal ROC number " <<dec<<iroc<<endl;
      }
      if( fDoBench ) fBench->Stop("physics_decode");
#endif
      return HED_ERR;
    }
    // Save position and length of each found ROC data block
    // Save position and length of each found ROC data block
    rocdat[iroc].pos  = pos;
    rocdat[iroc].len  = len;
    irn[nroc++] = iroc;
    pos += len+1;
  }

  if (fDebugFile) {
    *fDebugFile << "CodaDecode:: num rocs "<<dec<<nroc<<endl;
    for (Int_t i=0; i < nroc; i++) {
      Int_t iroc=irn[i];
      *fDebugFile << "   CodaDecode::   roc  num "<<iroc<<"   pos "<<rocdat[iroc].pos<<"     len "<<rocdat[iroc].len<<endl;
    }
  }

  return status;

}


// To initialize the THaSlotData member on first call to decoder
int CodaDecoder::init_slotdata(const THaCrateMap* map)
{
  // Update lists of used/clearable slots in case crate map changed
  if(!map) return HED_ERR;

  for (Int_t iroc = 0; iroc<MAXROC; iroc++) {
    if (  !map->crateUsed(iroc)  ) continue;

    for (Int_t islot=0; islot < MAXSLOT; islot++) {

      if ( !map->slotUsed(iroc,islot) ) continue;

	makeidx(iroc,islot);

    }
  }

  if (fDebugFile) *fDebugFile << "CodaDecode:: fNSlotUsed "<<fNSlotUsed<<endl;

  for( int i=0; i<fNSlotUsed; i++ ) {
    THaSlotData* crslot = crateslot[fSlotUsed[i]];
    int crate = crslot->getCrate();
    int slot  = crslot->getSlot();
    crslot->loadModule(map);
    if (fDebugFile) *fDebugFile << "CodaDecode::  crate, slot "<<crate<<"  "<<slot<<"   Dev type  = "<<crslot->devType()<<endl;
    if( !map->crateUsed(crate) || !map->slotUsed(crate,slot) ||
	!map->slotClear(crate,slot)) {
      for( int k=0; k<fNSlotClear; k++ ) {
	if( crslot == crateslot[fSlotClear[k]] ) {
	  for( int j=k+1; j<fNSlotClear; j++ )
	    fSlotClear[j-1] = fSlotClear[j];
	  fNSlotClear--;
	  break;
	}
      }
    }
    if( !map->crateUsed(crate) || !map->slotUsed(crate,slot)) {
      for( int j=i+1; j<fNSlotUsed; j++ )
	fSlotUsed[j-1] = fSlotUsed[j];
      fNSlotUsed--;
    }
  }


  return HED_OK;

}


//_____________________________________________________________________________
void CodaDecoder::dump(const UInt_t* evbuffer) const
{
  if( !evbuffer ) return;
  if ( !fDebugFile ) return;
  Int_t len = evbuffer[0]+1;
  Int_t type = evbuffer[1]>>16;
  Int_t num = evbuffer[4];
  *fDebugFile << "\n\n Raw Data Dump  " << hex << endl;
  *fDebugFile << "\n Event number  " << dec << num;
  *fDebugFile << "  length " << len << "  type " << type << endl;
  Int_t ipt = 0;
  for (Int_t j=0; j<(len/5); j++) {
    *fDebugFile << dec << "\n evbuffer[" << ipt << "] = ";
    for (Int_t k=j; k<j+5; k++) {
      *fDebugFile << hex << evbuffer[ipt++] << " ";
    }
  }
  if (ipt < len) {
    *fDebugFile << dec << "\n evbuffer[" << ipt << "] = ";
    for (Int_t k=ipt; k<len; k++) {
      *fDebugFile << hex << evbuffer[ipt++] << " ";
    }
    *fDebugFile << endl;
  }
  *fDebugFile<<dec<<endl;
}

//_____________________________________________________________________________
void CodaDecoder::CompareRocs(  )
{
  if (!fMap || !fDebugFile) return;
  *fDebugFile<< "Comparing cratemap rocs with found rocs"<<endl;
  for (Int_t i=0; i<nroc; i++) {
    Int_t iroc = irn[i];
    if (!fMap->crateUsed(iroc)) {
	  *fDebugFile << "ERROR  CompareRocs:: roc "<<iroc<<"  in data but not in map"<<endl;
    }
  }
  for (Int_t iroc1 = 0; iroc1<MAXROC; iroc1++) {
    if (  !fMap->crateUsed(iroc1)  ) continue;
    Int_t ifound=0;
    for( Int_t i=0; i<nroc; i++ ) {
      Int_t iroc2 = irn[i];
      if (iroc1 == iroc2) {
	ifound=1;
	break;
      }
    }
    if (!ifound) *fDebugFile << "ERROR: CompareRocs:  roc "<<iroc1<<" in cratemap but not found data"<<endl;
  }
}

//_____________________________________________________________________________
void CodaDecoder::ChkFbSlot( Int_t roc, const UInt_t* evbuffer,
				  Int_t ipt, Int_t istop )
{
  const UInt_t* p      = evbuffer+ipt;    // Points to ROC ID word (1 before data)
  const UInt_t* pstop  =evbuffer+istop;   // Points to last word of data in roc
  while (p++ < pstop) {
    Int_t slot = (UInt_t(*p))>>27;  // A "self-reported" slot.
    Int_t index = MAXSLOT*roc + slot;
    if ((slot > 0) && (index >=0 && index < MAXROC*MAXSLOT)) fbfound[index]=1;
  }
}

//_____________________________________________________________________________
void CodaDecoder::ChkFbSlots()
{
  // This checks the fastbus slots to see if slots are appearing in both the
  // data and the cratemap.  If they appear in one but not the other, a warning
  // is issued, which usually means the cratemap is wrong.
  Int_t slotstat[MAXROC*MAXSLOT];
  for (Int_t iroc=0; iroc<MAXROC; iroc++) {
    if ( !fMap->isFastBus(iroc) ) continue;
    for (Int_t islot=0; islot<MAXSLOT; islot++) {
      Int_t index = MAXSLOT*iroc + islot;
      slotstat[index]=0;
      if (fbfound[index] && fMap->slotUsed(iroc, islot)) {
	  if (fDebugFile) *fDebugFile << "FB slot in cratemap and in data.  (good!).  roc = "<<iroc<<"   slot = "<<islot<<endl;
	  slotstat[index]=1;
      }
      if ( !fbfound[index] && fMap->slotUsed(iroc, islot)) {
	if (fDebugFile) *fDebugFile << "FB slot NOT in data, but in cratemap  (bad!).  roc = "<<iroc<<"   slot = "<<islot<<endl;
	slotstat[index]=2;
      }
      if ( fbfound[index] && !fMap->slotUsed(iroc, islot)) {
	if (fDebugFile) *fDebugFile << "FB slot in data, but NOT in cratemap  (bad!).  roc = "<<iroc<<"   slot = "<<islot<<endl;
	slotstat[index]=3;
      }
    }
  }
  for (Int_t iroc=0; iroc<MAXROC; iroc++) {
    if ( !fMap->isFastBus(iroc) ) continue;
    for (Int_t islot=0; islot<MAXSLOT; islot++) {
      Int_t index = MAXSLOT*iroc + islot;
      if (slotstat[index]==2) cout << "Decoder:: WARNING:  Fastbus module in (roc,slot) = ("<<iroc<<","<<islot<<")  found in cratemap but NOT in data !"<<endl;
    }
  }
  for (Int_t iroc=0; iroc<MAXROC; iroc++) {
    if ( !fMap->isFastBus(iroc) ) continue;
    for (Int_t islot=0; islot<MAXSLOT; islot++) {
      Int_t index = MAXSLOT*iroc + islot;
      if (slotstat[index]==3) cout << "Decoder:: WARNING:  Fastbus module in (roc,slot) = ("<<iroc<<","<<islot<<")  found in data but NOT in cratemap !"<<endl;
    }
  }
}


//_____________________________________________________________________________
void CodaDecoder::SetRunTime( ULong64_t tloc )
{
  // Set run time and re-initialize crate map (and possibly other
  // database parameters for the new time.

  if( fRunTime == tloc )
    return;
  fRunTime = tloc;
  fNeedInit = true;  // force re-init
}

}

//_____________________________________________________________________________
ClassImp(Decoder::CodaDecoder)
