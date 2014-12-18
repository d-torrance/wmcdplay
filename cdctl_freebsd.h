// cdctl.h - CDCtl class provides easy control of cd audio functions
// 05/09/98  Release 1.0 Beta1
// Copyright (C) 1998  Sam Hawker <shawkie@geocities.com>
// This software comes with ABSOLUTELY NO WARRANTY
// This software is free software, and you are welcome to redistribute it
// under certain conditions
// See the README file for a more complete notice.

// Although cdctl.h is an integral part of wmcdplay, it may also be distributed seperately.

// Change this define to alter the size of forward and backward skips (in frames)
// Yes, I know this should really be a method of CDCtl
#define _CDCTL_SKIP_SIZE 1125

// Try defining some of these. They may improve performance or reliability
// (or just plain make it work)
// #define _CDCTL_STOP_BEFORE_PLAY
// #define _CDCTL_START_BEFORE_PLAY
// #define _CDCTL_SOFT_STOP

// Define this if it stops after each track
#define _CDCTL_SENSITIVE_EOT
// If it still stops for a while between tracks, increase this (0-75 is a sensible range)
#define _CDCTL_SENSITIVITY 0

#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/cdio.h>
#include <arpa/inet.h>
#define CD_MSF_OFFSET	150

// CD status values
#define ssData     0
#define ssStopped  1
#define ssPlaying  2
#define ssPaused   3
#define ssNoCD     4
#define ssTrayOpen 5

// Audio command values
#define acStop   0
#define acPlay   1
#define acPause  2
#define acResume 3
#define acPrev   4
#define acNext   5
#define acRewd   6
#define acFFwd   7
#define acEject  8
#define acClose  9

// Track selection values (what to do when I've played the requested track)
// Note:      Track selection is not perfect - so use tsNone if you want to avoid trouble.
//            Basically, if we receive a CDROM_AUDIO_COMPLETED status, then we have to decide what to do.
//            If we think the last play command was ours (Next/Prev/FFwd/Rewd don't count), then we do something,
//            depending on the current track selection mode.
// Failures:  Sometimes we may think we sent the last play command when we did not (if we didn't see play stop in
//            in between).
//            If another application is polling the status, it may receive the CDROM_AUDIO_COMPLETED we are looking
//            for, and we will not, so will think play was stopped manually.
//            Similarly, we may read the CDROM_AUDIO_COMPLETED status when we don't want it, such that the other
//            application never sees it.
// Verdict:   Linux audio cdrom handling is broken.
// Update:    New define _CDCTL_SENSITIVE_EOT may help in cases where CDROM_AUDIO_COMPLETED is not being returned
//            correctly. It may, however, interfere with other running cd players.

// Update:    I think this works like a dream now. Even with many cd players sharing a cdrom. Let me know if not!!

#define tsNone      0    // Just stop
#define tsRepeat    1    // Play it again
#define tsNext      2    // Play next track (stop at end of CD)
#define tsRepeatCD  3    // Play next track (start from first track if end is reached)
#define tsRandom    4    // Play a track at random

class CDCtl
{
public:
   CDCtl(char *dname){
      device=(char *)malloc(sizeof(char)*(strlen(dname)+1));
      strcpy(device,dname);
      srand(getpid());
      tracksel=tsRandom;
      tskOurPlay=false;

      if(cdfdopen=(cdfd=open(device,O_RDONLY | O_NONBLOCK))!=-1){
         status_state=ssNoCD;
         status_track=0;
         status_pos=0;
         cd_trklist=NULL;
         doStatus();
         readVolume();
      }
   }
   ~CDCtl(){
      if(cdfdopen){
         close(cdfd);
         if(device!=NULL)
            free(device);
         if(cd_trklist!=NULL)
            free(cd_trklist);
      }
   }
   bool openOK(){
      return cdfdopen;
   }
   void doAudioCommand(int cmd){
      if(cdfdopen){
         int newtrk=status_track;
	 switch(cmd){
	  case acStop:

             #ifdef _CDCTL_SOFT_STOP
             ioctl(cdfd,CDIOCSTART);
             #endif
             #ifndef _CDCTL_SOFT_STOP
             ioctl(cdfd,CDIOCSTOP);
             #endif
             tskOurPlay=false;

          break;
	  case acPlay:
             status_state=ssPlaying;
             select(status_track);
             tskOurPlay=true;
          break;
          case acPause:
             ioctl(cdfd,CDIOCPAUSE);
          break;
	  case acResume:
             ioctl(cdfd,CDIOCRESUME);
          break;
	  case acPrev:
             newtrk--;
             if(newtrk<0)
                newtrk=cd_tracks-1;
             select(newtrk);
          break;
 	  case acNext:
             newtrk++;
             if(newtrk>cd_tracks-1)
                newtrk=0;
             select(newtrk);
          break;
	  case acRewd:
	     if(status_pos>cd_trklist[status_track].track_start+_CDCTL_SKIP_SIZE){
                status_pos-=_CDCTL_SKIP_SIZE;
                play();
             }
          break;
          case acFFwd:
	    if(status_pos<cd_trklist[status_track].track_start+cd_trklist[status_track].track_len-_CDCTL_SKIP_SIZE){
               status_pos+=_CDCTL_SKIP_SIZE;
               play();
            }
          break;
	  case acEject:
             if(ioctl(cdfd,CDIOCEJECT))
                status_state=ssNoCD;
             else
                status_state=ssTrayOpen;
          break;
	  case acClose:
             ioctl(cdfd,CDIOCCLOSE);
             status_state=ssNoCD;
          break;
         }
         doStatus();
      }
   }
   void doStatus(){
      if(cdfdopen){
         struct ioc_read_subchannel sc;
         struct cd_sub_channel_info csci;
         sc.address_format=CD_MSF_FORMAT;
         sc.track = 0;
         sc.data=&csci;
         sc.data_len=sizeof(csci);
         sc.data_format=CD_CURRENT_POSITION;
         if(ioctl(cdfd, CDIOCREADSUBCHANNEL, &sc)){
            if(status_state!=ssNoCD)
               status_state=ssTrayOpen;
	    status_track=0;
            status_pos=0;
            tskOurPlay=false;
         }
         else{
            if(status_state==ssNoCD || status_state==ssTrayOpen)
	       readTOC();
            int start,now,stop;
            switch(csci.header.audio_status){
             case CD_AS_PLAY_IN_PROGRESS:
                if(status_state==ssStopped)
                   tskOurPlay=false;
                status_state=ssPlaying;
             break;
             case CD_AS_PLAY_PAUSED:
                if(status_state==ssStopped)
                   tskOurPlay=false;
                status_state=ssPaused;
             break;
             case CD_AS_PLAY_COMPLETED:
	        if(tskOurPlay){
                   status_state=ssPlaying;
	           selecttrack();
	           doStatus();
	           return;
	        }
	        else
	           status_state=ssStopped;
	     break;
             default:

                #ifdef _CDCTL_SENSITIVE_EOT
                if(tskOurPlay){
                   start = cd_trklist[status_track].track_start;
                   stop = start + cd_trklist[status_track].track_len - _CDCTL_SENSITIVITY;
                   now = ((csci.what.position.absaddr.msf.minute) * 60 + csci.what.position.absaddr.msf.second) * 75 + csci.what.position.absaddr.msf.frame - CD_MSF_OFFSET;
	           if(now>0 && (now<start || now>=stop)){
                      status_state=ssPlaying;
                      selecttrack();
                      doStatus();
                      return;
                   }
                   else
                      status_state=ssStopped;
	        }
                else
                #endif

                   status_state=ssStopped;
            }
            trackinfo(&csci);
            if(cd_trklist[status_track].track_data)
               status_state=ssData;
         }
      }
   }
   void setVolume(int l, int r){
      if(cdfdopen){
         struct ioc_vol vol;
         vol.vol[0]=l;
         vol.vol[1]=r;
         vol.vol[2]=0;
         vol.vol[3]=0;
         ioctl(cdfd,CDIOCSETVOL,&vol);
         readVolume();
      }
   }
   void readVolume(){
      if(cdfdopen){
         struct ioc_vol vol;
         ioctl(cdfd,CDIOCGETVOL,&vol);
         status_volumel=vol.vol[0];
         status_volumer=vol.vol[1];
      }
   }
   int getVolumeL(){
      return status_volumel;
   }
   int getVolumeR(){
      return status_volumer;
   }
   void setTrackSelection(int ts){
      tracksel=ts;
   }
   int getTrackSelection(){
      return tracksel;
   }
   char *getDevName(){
      return device;
   }
   int getCDTracks(){
      return cd_tracks;
   }
   int getCDLen(){
      return cd_len;
   }
   int getTrackStart(int trk){
      return cd_trklist[trk-1].track_start;
   }
   int getTrackLen(int trk){
      return cd_trklist[trk-1].track_len;
   }
   bool getTrackData(int trk){
      return cd_trklist[trk-1].track_data;
   }
   int getStatusState(){
      return status_state;
   }
   int getStatusTrack(){
      return status_track+1;
   }
   int getStatusPosAbs(){
      return status_pos-cd_trklist[0].track_start;
   }
   int getStatusPosRel(){
      return status_pos-cd_trklist[status_track].track_start;
   }
private:
   void readTOC(){
      if(cd_trklist!=NULL)
         free(cd_trklist);
      struct ioc_toc_header hdr;
      ioctl(cdfd, CDIOREADTOCHEADER, &hdr);
      cd_tracks=hdr.ending_track;
      cd_trklist=(struct CDTrack *)malloc(cd_tracks*sizeof(struct CDTrack));
#if defined(__FreeBSD__) || defined(__FreeBSD_kernel__)
	struct ioc_read_toc_entry te;
      
	te.data_len = (cd_tracks + 1) * sizeof(struct cd_toc_entry);
	te.data = (struct cd_toc_entry *)malloc(te.data_len);
	te.address_format = CD_LBA_FORMAT;
	te.starting_track = 0;
	ioctl(cdfd, CDIOREADTOCENTRYS, &te);
	for(int i = 0; i < cd_tracks; i++) {
		cd_trklist[i].track_data = te.data[i].control & 4 ? true : false;
		cd_trklist[i].track_start = ntohl(te.data[i].addr.lba);
		cd_trklist[i].track_len = ntohl(te.data[i + 1].addr.lba)
			- cd_trklist[i].track_start;
	}
	cd_len = ntohl(te.data[cd_tracks].addr.lba);
	free(te.data);
#else
      struct cdrom_tocentry te;
      int prev_addr=0;
      
      for(int i=0;i<=cd_tracks;i++){
         if(i==cd_tracks)
            te.cdte_track=CDROM_LEADOUT;
         else
            te.cdte_track=i+1;
         te.cdte_format=CDROM_MSF;    // I think it is ok to read this as LBA, but for a quiet life...
         ioctl(cdfd, CDROMREADTOCENTRY, &te);
         int this_addr=((te.cdte_addr.msf.minute * 60) + te.cdte_addr.msf.second) * 75 + te.cdte_addr.msf.frame - CD_MSF_OFFSET;
         if(i>0)
	    cd_trklist[i-1].track_len = this_addr - prev_addr - 1;
         prev_addr=this_addr;
         if(i<cd_tracks){
            cd_trklist[i].track_data = te.cdte_ctrl & CDROM_DATA_TRACK ? true : false;
	    cd_trklist[i].track_start = this_addr;
         }
         else
            cd_len = this_addr;
      }
#endif
   }
   void trackinfo(struct cd_sub_channel_info *subchnl){
      int currenttrack = status_track;

      if(status_state==ssPlaying || status_state==ssPaused){
         status_pos=((subchnl->what.position.absaddr.msf.minute) * 60 + subchnl->what.position.absaddr.msf.second) * 75 + subchnl->what.position.absaddr.msf.frame - CD_MSF_OFFSET;
         for(status_track=0;status_track<cd_tracks;status_track++){
            if(status_pos<cd_trklist[status_track].track_start+cd_trklist[status_track].track_len) {
               if (status_track != currenttrack) {
                  status_track = currenttrack;
               }
               break;
            }
         }
      }
   }
   void play(){
      struct ioc_play_msf pmsf;
      int abs0=status_pos + CD_MSF_OFFSET;
      int abs1=cd_trklist[status_track].track_start + cd_trklist[status_track].track_len - 1 + CD_MSF_OFFSET;
      pmsf.start_m=abs0/(75*60);
      pmsf.end_m=abs1/(75*60);
      pmsf.start_s=(abs0%(75*60))/75;
      pmsf.end_s=(abs1%(75*60))/75;
      pmsf.start_f=abs0%75;
      pmsf.end_f=abs1%75;

      #ifdef _CDCTL_STOP_BEFORE_PLAY
      ioctl(cdfd,CDIOCSTOP);
      #endif
      #ifdef _CDCTL_START_BEFORE_PLAY
      ioctl(cdfd,CDIOCSTART);
      #endif

      ioctl(cdfd,CDIOCPLAYMSF,&pmsf);
   }
   void select(int trk){
      status_track=trk;
      status_pos=cd_trklist[status_track].track_start;
      if(status_state==ssPlaying){
         if(cd_trklist[status_track].track_data){

             #ifdef _CDCTL_HARD_STOP
             ioctl(cdfd,CDIOCSTOP);
             #endif
             #ifndef _CDCTL_HARD_STOP
             ioctl(cdfd,CDIOCSTART);
             #endif
             tskOurPlay=false;

         }
         else
            play();
      }
   }
   void selecttrack(){
      int newtrk=status_track;
      do{
         switch(tracksel){
          case tsNone:
             tskOurPlay=false;
             return;
          break;
          case tsRepeat:
             // do nothing
          break;
          case tsNext:
             newtrk++;
             if(newtrk>=cd_tracks){
                tskOurPlay=false;
                return;
             }
          break;
          case tsRepeatCD:
             newtrk++;
             if(newtrk>=cd_tracks)
                newtrk=0;
          break;
          case tsRandom:
             newtrk+=(int)((cd_tracks-1)*(float)rand()/RAND_MAX+1);
             if(newtrk>=cd_tracks)
                newtrk-=cd_tracks;
          break;
         }
      }  while(cd_trklist[newtrk].track_data);
      select(newtrk);
      play();
   }
   int cdfd;
   int cdfdopen;
   char *device;
   int tracksel;
   bool tskOurPlay;

   struct CDTrack{
      int track_start;
      int track_len;
      bool track_data;
   };

   int cd_tracks;
   int cd_len;
   struct CDTrack *cd_trklist;
   int status_state;
   int status_track;
   int status_pos;
   int status_volumel;
   int status_volumer;
};
