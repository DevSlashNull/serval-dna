/*
Serval Mesh Software
Copyright (C) 2010-2012 Paul Gardner-Stephen

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
  We wish to be able to synchronise Rhizome repositories over asynchonous and/or
  low-bandwidth links.

  One of the challenges is to maximise the usage of the messages, and minimise the
  number of messages, because they can be quite expensive (e.g., sms or satellite
  sms which can cost $0.25 per 140bytes (sms) or $0.25 - $1.00 per 160 bytes 
  (satellite sms).

  If there are very few new items, it may be more efficient to just send them
  rather than negotiate about them because of the high per-message cost.

*/

#include <stdlib.h>
#include "serval.h"
#include "rhizome.h"
#include "cli.h"
#include "overlay_buffer.h"
#include "monitor-client.h"
#include "conf.h"
#include "str.h"

#include <dirent.h>

#define RDA_MSG_BARS_RAW 0x01
#define RDA_MSG_MANIFESTS 0x02

struct rhizome_direct_async_channel_state {
  // Last rhizome database insertion time that we 
  // have queued new manifests for
  uint64_t lastInsertionTime;
  // Last outbound message number
  uint64_t lastTXMessageNumber;

  // Number of manifests queued for announcement to
  // the far end.
  int queuedManifests;
  // Time last manifest was added to the queue.
  // Used with conf.rhizome_direct.asyncchannel.*.settle_time
  uint64_t lastManifestQueueTime;
  // Time first unannounced manifest was added to the queue
  // Used with conf.rhizome_direct.asyncchannel.*.max_settle_time
  uint64_t firstManifestQueueTime;
};

struct rhizome_direct_async_channel_state channel_states[16];

int rhizome_direct_async_load_state()
{
  int i;
  char filename[1024];

  DEBUGF("There are %d RD async channels.",config.rhizome.direct.channels.ac);
  for(i=0;i<config.rhizome.direct.channels.ac;i++) {
    /* All fields zero is the correct default state */
    bzero(&channel_states[i],sizeof(channel_states[i]));

    if (config.rhizome.direct.channels.av[i].value.out_path) {
      snprintf(filename,1024,"%s/queued_manifests",
	       config.rhizome.direct.channels.av[i].value.out_path);
      struct stat s;
      if (!stat(filename,&s)) {
	channel_states[i].queuedManifests=s.st_size/RHIZOME_BAR_BYTES;
	channel_states[i].firstManifestQueueTime=1000LL*s.st_ctime;
	channel_states[i].lastManifestQueueTime=1000LL*s.st_mtime;
      }
      snprintf(filename,1024,"%s/state",
	       config.rhizome.direct.channels.av[i].value.out_path);
      FILE *f=fopen(filename,"r");
      if (f) {
	fscanf(f,"%lld:%lld",  
	       &channel_states[i].lastInsertionTime,
	       &channel_states[i].lastTXMessageNumber);
	fclose(f);
      }
    }
    if (config.debug.rhizome_async)
      DEBUGF("RD channel #%d state: %lld:%lld:%d:%lld:%lld",
	     i,
	     channel_states[i].firstManifestQueueTime,
	     channel_states[i].lastManifestQueueTime,
	     channel_states[i].queuedManifests,
	     channel_states[i].lastInsertionTime,
	     channel_states[i].lastTXMessageNumber);	   
  }

  return 0;
}

char interval_description[128];
char *describe_time_interval_ms(int64_t interval)
{
  int futureP=0;
  if (!interval) return "right now";
  if (interval<0) { futureP=1; interval=-interval; }
  if (interval<400) snprintf(interval_description,128,"%lld milliseconds",interval);
  else if (interval<60*1000) snprintf(interval_description,128,"%.1f seconds",interval/1000.0);
  else if (interval<60*(60*1000)) snprintf(interval_description,128,"%.1f minutes",interval/(60*1000.0));
  else if (interval<24*(60*60*1000)) snprintf(interval_description,128,"%.2f hours",interval/(60*60*1000.0));
  else if (interval<365.24*(24*60*60*1000)) snprintf(interval_description,128,"%.2f days",interval/(24*60*60*1000.0));
  else if (interval<1000*365.24*24*60*60*1000) snprintf(interval_description,128,"%.2f years",interval/(365.24*24*60*60*1000.0));
  else if (interval<1000000*365.24*24*60*60*1000) snprintf(interval_description,128,"%.2fK years",interval/(365.24*24*60*60*1000.0*1000));
  else snprintf(interval_description,128,"%.2fM years",interval/(365.24*60*60*1000.0*1000000));
  if (futureP) strcat(interval_description," from now");
  else  strcat(interval_description," ago");
  return interval_description;
}

void rhizome_direct_async_bundlescan()
{
  int i;
  uint64_t oldest_interesting_time=gettime_ms();
  for(i=0;i<config.rhizome.direct.channels.ac;i++)
    if (channel_states[i].lastInsertionTime+1<oldest_interesting_time)
      oldest_interesting_time=channel_states[i].lastInsertionTime+1;
  if (config.debug.rhizome_async)
    DEBUGF("Examinining rhizome bundles inserted since %lld (%s)",
	   oldest_interesting_time,
	   describe_time_interval_ms((gettime_ms()-oldest_interesting_time)));
  // XXX Go through rhizome database looking at insertion times
    sqlite_retry_state retry = SQLITE_RETRY_STATE_DEFAULT;
  sqlite3_stmt *statement
    = sqlite_prepare(&retry, "SELECT rowid,inserttime FROM manifests"
		     " WHERE inserttime>=%lld ORDER BY inserttime", 
		     oldest_interesting_time);
  if (!statement)
    return;
  while (sqlite_step_retry(&retry, statement) == SQLITE_ROW) {
    char manifest[1024];    
    unsigned long long rowid=-1;
    int64_t insertTime=0;
    sqlite3_blob *blob;
    if (sqlite3_column_type(statement, 0)==SQLITE_INTEGER)
      rowid = sqlite3_column_int64(statement, 0);
    if (sqlite3_column_type(statement, 1)==SQLITE_INTEGER)
      insertTime = sqlite3_column_int64(statement, 1);
    int ret;
    do ret = sqlite3_blob_open(rhizome_db, "main", "MANIFESTS", "manifest", 
			       rowid, 0, &blob);
    while (sqlite_code_busy(ret) && sqlite_retry(&retry, "sqlite3_blob_open"));
    if (ret != SQLITE_OK) {
      WHYF("sqlite3_blob_open() failed, %s", sqlite3_errmsg(rhizome_db));
      continue;
    }
    int manifestSize=sqlite3_blob_bytes(blob);
    if (manifestSize<=1024) {
      if(sqlite3_blob_read(blob,manifest,manifestSize,0)
	 !=SQLITE_OK) {
	sqlite3_blob_close(blob);
	WHYF("sqlite3_blob_read() failed, %s", sqlite3_errmsg(rhizome_db));
	continue;
      }

      if (config.debug.rhizome_async)
	DEBUGF("Read manifest of %d bytes, inserted %s",
	       manifestSize,
	       describe_time_interval_ms(gettime_ms()-insertTime));

      rhizome_manifest m;
      bzero(&m,sizeof(m));
      if (rhizome_read_manifest_file(&m, manifest, manifestSize) != -1) {
	rhizome_direct_sync_bundle_added(&m,insertTime);
      }
    }
    sqlite3_blob_close(blob);

  }
  sqlite3_finalize(statement);
}

int rhizome_direct_async_setup()
{
  // XXX Load state of channels, i.e.:
  // - last TX message number, 
  // - last dispatch time
  // - last rhizome inserttime dealt with
  rhizome_direct_async_load_state();

  /* Add any bundles that have arrived since last run to be added to the 
     queues. */
  rhizome_direct_async_bundlescan();

  // Go through received messages and see if there is a complete transmission.
  // Actually, don't bother, since rhizome_direct_async_periodic() will do
  // this for us.

  return 0;
}

void rhizome_direct_async_periodic(struct sched_ent *alarm)
{
  if (config.debug.rhizome_async) DEBUG("called.");

  // Check if any channels need flushing
  int i;
  for(i=0;i<config.rhizome.direct.channels.ac;i++)
    {
      if (channel_states[i].queuedManifests) {
	int flushQueue=0;
	int64_t waitingTime=gettime_ms()-channel_states[i].firstManifestQueueTime;
	if (!channel_states[i].firstManifestQueueTime) waitingTime=0;
	int64_t settleTime=gettime_ms()-channel_states[i].lastManifestQueueTime;
	if (!channel_states[i].lastManifestQueueTime)
	  settleTime=0;
	DEBUGF("waitingTime = %lld (%lld)",
	       waitingTime,channel_states[i].firstManifestQueueTime);
	// Channel has BARs queued -- check if queue needs flushing.

	// Flush if there are too many BARs queued up
	if (channel_states[i].queuedManifests
	    >=config.rhizome.direct.channels.av[i].value.max_pending) 
	  {
	    if (config.debug.rhizome_async) 
	      DEBUGF("Flushing channel #%d queue due to max_pending"
		     " (%d waiting)",i,channel_states[i].queuedManifests);	    
	    flushQueue=1;
	  }
	// Flush if BARs have been queued for at least max_settle_time,
	// irrespective of how many are queued.
	if (waitingTime>=config.rhizome.direct.channels.av[i].value.max_settle_time)
	  {
	    if (config.debug.rhizome_async) 
	      DEBUGF("Flushing channel #%d queue due to max_settle_time"
		     " (BARs waiting since %s)",i,
		     describe_time_interval_ms(waitingTime));
	    flushQueue=1;
	  }
	// Flush if BARs have been waiting, and no new BARs have been queued 
	// for atleast settle_time
	if (settleTime>=config.rhizome.direct.channels.av[i].value.settle_time)
	  {
	    if (config.debug.rhizome_async) 
	      DEBUGF("Flushing channel #%d queue due to settle_time"
		     " (BARs waiting since %s)",i,
		     describe_time_interval_ms(settleTime));
	    flushQueue=1;
	  }
	if (config.debug.rhizome_async) 
	  DEBUGF("Will%s flush channel #%d queue",flushQueue?"":"not",i);
	if (flushQueue) rhizome_direct_async_flush_queue(i);	
      }
    }

  // XXX Check for new messages arriving on any channel

  // Update next call time
  alarm->alarm = gettime_ms()+1000;
  alarm->deadline = alarm->alarm + 10000;
  schedule(alarm);
  return;
}

int rhizome_direct_process_reconstructed_message(int channel,unsigned char *message,int len)
{
  if (config.debug.rhizome_async)
    DEBUGF("Processing RD async message of length %d",len);

  if (len<(4+4+4)) {
    DEBUGF("RD async message too short (minimum header length = 12 bytes)");
  }

  dump("RD async message",message,len);

  struct overlay_buffer *ob=ob_static(message,len);

  uint64_t timeRangeStart,timeRangeEnd;
  /* 64 bits of time range start */
  /* 32 bits of time range */
  timeRangeStart=((unsigned long long )ob_get_ui32(ob))<<32LL;
  timeRangeStart|=(unsigned int)ob_get_ui32(ob);
  timeRangeEnd=timeRangeStart+(unsigned int)ob_get_ui32(ob);

  if (config.debug.rhizome_async) {
    char end[1024];
    snprintf(end,1024,"%s",describe_time_interval_ms(gettime_ms()-timeRangeEnd));
    DEBUGF("RD async message covers from %s to %s",
	   describe_time_interval_ms(gettime_ms()-timeRangeStart),
	   end);
  }

  char filename[1024];
  snprintf(filename,1024,"%s/received_announcements",
	   config.rhizome.direct.channels.av[channel].value.out_path);
  FILE *f=fopen(filename,"a+");

  int messageType=ob_get(ob);
  if (config.debug.rhizome_async)
    DEBUGF("RD Async message type = 0x%02x",messageType);
  switch(messageType) {
  case RDA_MSG_BARS_RAW:
    /* Raw BARs */
    while(ob->position+RHIZOME_BAR_BYTES<=len) {
    if (config.debug.rhizome_async)
      dump("received IHAVE BAR",&ob->bytes[ob->position],RHIZOME_BAR_BYTES);
    /* Remember that far end has announced this BAR */
    if (f) fwrite(&ob->bytes[ob->position],RHIZOME_BAR_BYTES,1,f);    
    DEBUGF("See whether we want to receive this bundle");
    /* Look at next BAR */
    ob->position+=RHIZOME_BAR_BYTES;
    }
  default:
  if (config.debug.rhizome_async)
    DEBUGF("Ignoring unknown or unimplemented message type.");
  }

  fclose(f);
  ob_free(ob);

  DEBUGF("Not implemented!");
  return -1;
}

struct rdasync_message_status {
  char *filename;
  int byte_count;
  unsigned short sequence;
  char startP;
  char endP;
};

int compare_rsams(const void *a, const void *b)
{
  const struct rdasync_message_status 
    *aa=(struct rdasync_message_status *)a,
    *bb=(struct rdasync_message_status *)b;
  if (aa->sequence<bb->sequence) return -1;
  if (aa->sequence>bb->sequence) return 1;
  return 0;
}

int rhizome_direct_async_readmessage(int channel,char *filename,
				     struct rdasync_message_status *m,
				     unsigned char *bytes, int max_bytes)
{
  /* Sanity check the inbound message */
  struct stat s;
  if (stat(filename,&s)) return -1;
  int64_t size=s.st_size;
  if (size>0x10000) return -1;

  unsigned char *data=malloc(size);
  if (!data) return -1;

  FILE *f=fopen(filename,"r");
  if (!f) {
    DEBUGF("Could not open rhizome direct async message file '%s'",filename);
    free(data); 
    return -1;
  }
  if (fread(data,size,1,f)!=1)
    {
      DEBUGF("Could not read rhizome direct async message data from '%s'",filename);
      fclose(f);
      free(data); 
      return -1;
    }
  fclose(f);
  if (config.rhizome.direct.channels.av[channel].value.alphabet_size!=256)
    {
      // Decode message of reduced alphabet size.
      // currently not implemented
      DEBUGF("Non 8-bit clean rhizome direct channels not yet supported"
	     " (channel #%d alphabet_size=%d)",
	     channel,
	     config.rhizome.direct.channels.av[channel].value.alphabet_size);
      free(data);
      return -1;
    }
  if (size<2) {
    DEBUGF("Decoded rhizome direct message from '%s' too short"
	   " (must be at least 2 bytes)",filename);
    free(data);
    return -1;
  }

  unsigned short header_short=(data[0]<<8)+data[1];
  m->sequence=header_short&0x3fff;
  m->startP=(header_short&0x4000)?1:0;
  m->endP=(header_short&0x8000)?1:0;
  m->byte_count=size-2;
  m->filename=strdup(filename);

  if (config.debug.rhizome_async)
    DEBUGF("Saw RD async message #%d startP=%d, endP=%d, size=%d",
	   m->sequence,m->startP,m->endP,m->byte_count);

  if (bytes&&m->byte_count<=max_bytes) bcopy(&data[2],bytes,m->byte_count);

  free(data);
  return 0;
}

int rhizome_direct_async_messagescan()
{
  int channel;
  DIR *dir;
  struct dirent *dirent;
  struct rdasync_message_status message_list[RDASYNC_MAX_MESSAGE_FRAGMENTS];
  int message_count;

  if (config.debug.rhizome_async)
    DEBUGF("Scanning for received rhizome direct async messages");

  for(channel=0;channel<config.rhizome.direct.channels.ac;channel++) 
    {
      message_count=0;

      dir=opendir(config.rhizome.direct.channels.av[channel].value.in_path);
      if (!dir) continue;
      while((dirent=readdir(dir))!=NULL) {
	if (dirent->d_type==DT_REG) {
	  // consider file
	  if (dirent->d_name[0]&&dirent->d_name[0]!='.')
	    if (!strncasecmp("tx.",dirent->d_name,3)) {
	      char filename[1024];
	      snprintf(filename,1024,"%s/%s",
		       config.rhizome.direct.channels.av[channel].value.in_path,
		       dirent->d_name);
	      if (config.debug.rhizome_async)
		DEBUGF("Possible in-bound message in for channel#%d in %s",
		       channel,filename);

	      // Get information about file 
	      if (!rhizome_direct_async_readmessage(channel,filename,
						    &message_list[message_count],
						    NULL,0)) {
		if (message_list[message_count].startP
		    &&message_list[message_count].endP
		    &&message_list[message_count].byte_count<=1024*1024) {		  
		  // Single piece message -- process it now.
		  unsigned char message_buffer[message_list[message_count].byte_count];
		  rhizome_direct_async_readmessage(channel,filename,
						   &message_list[message_count],
						   message_buffer,
						   message_list[message_count].byte_count);
		  rhizome_direct_process_reconstructed_message
		    (channel,message_buffer,message_list[message_count].byte_count);

		  if (config.debug.rhizome_async)
		    DEBUGF("unlink(%s)",message_list[message_count].filename);
		  unlink(message_list[message_count].filename);
		  free(message_list[message_count].filename);
		  message_list[message_count].filename=NULL;
		} else
		  message_count++;
	      }
	    }
	}
      }
      closedir(dir);
    
      // Sort the list to deal with out-of-order delivery
      qsort(&message_list[0],message_count,sizeof(struct rdasync_message_status),
	    compare_rsams);
      
      // Scan for any complete messages.
      // XXX Doesn't handle duplicate reception yet.
      int i,j;
      int dud,total_bytes;
      for(i=0;i<message_count;i++) {
	if (message_list[i].startP) {
	  dud=0;
	  total_bytes=0;
	  total_bytes=message_list[i].byte_count;
	  for(j=i+1;j<message_count&&message_list[j].endP==0;j++)
	    if (message_list[j].sequence
		!=((message_list[i].sequence+(j-i))&0x3fff)) {
	      // this is not the next piece we need, so abort
	      dud=1;
	      break;
	    } else { 
	      total_bytes+=message_list[j].byte_count;
	    }
	  if ((!dud)&&message_list[j].endP&&
	      message_list[j].sequence
	      ==((message_list[i].sequence+(j-i))&0x3fff)) {
	    
	    // We have a complete message
	    
	    // Add length of final fragment
	    total_bytes+=message_list[j].byte_count;

	    // Skip messages >1MB
	    if (total_bytes>1024*1024) {
	      INFOF("Skipping rhizome direct message >1MB");
	    } else {       
	      INFOF("Reconstructing rhizome direct message of %d bytes",total_bytes);
	      unsigned char *message_buffer=malloc(total_bytes);
	      int k,offset=0;
	      struct rdasync_message_status message;
	      for(k=i;k<=j;k++) {
		if (rhizome_direct_async_readmessage(channel,message_list[k].filename,
						     &message,
						     &message_buffer[offset],
						     total_bytes-offset)) {
		  
		}
		else offset+=message.byte_count;
	      }
	      if(offset==total_bytes) {
		if (config.debug.rhizome_async)
		  DEBUGF("Processing rhizome direct async message of %d fragments",j-i+1);
		rhizome_direct_process_reconstructed_message
		  (channel,message_buffer,total_bytes);
	      } else {
		DEBUGF("Failed to reconstruct multi-fragment message"
		       " (got length = %d instead of %d).",
		       offset,total_bytes);
	      }
	      free(message_buffer);
	    }
	  }
	}
      }
      
      // Free up list
      for(i=0;i<message_count;i++) {
	if (message_list[i].filename) free(message_list[i].filename);
	message_list[i].filename=NULL;
      }
    }

  return 0;
}

// Called when the monitor command "rdasync check" is issued.  This tells us
// to look for newly received messages in the in-bound spool directories for the
// rhizome direct async channels.  Also check for any recently arrived bundles.
int monitor_rhizome_direct_async_rx(int argc, const char *const *argv, 
					   const struct command_line_option *o, 
					   void *context)
{
  struct monitor_context *c=context;
  char msg[256];
  snprintf(msg, sizeof(msg), "\nOK:\n");
  write_str(c->alarm.poll.fd, msg);

  // Check for any new bundles
  rhizome_direct_async_bundlescan();

  // Check for any new messages
  rhizome_direct_async_messagescan();

  return 0;
}

// Called whenever a bundle is stored in our rhizome database.
// We use this notification to add bundles to our knowledge of those that need
// to be sent to our async peers.
// We need to make sure that when we receive a manifest from an async peer that
// we don't bother announcing it back to that peer, and generate unnecessary 
// message traffic!
int rhizome_direct_async_does_far_end_have(int channelNumber,
					   unsigned char *bid,
					   long long version) 
{
  // XXX This uses a linear search which will be very inefficient once
  // we start synchronising lots of bundles.
  char filename[1024];
  snprintf(filename,1024,"%s/received_announcements",
	   config.rhizome.direct.channels.av[channelNumber].value.out_path);
  FILE *f=fopen(filename,"r");
  if (!f) return 0;
  while(!feof(f)) {
    unsigned char bar[RHIZOME_BAR_BYTES];
    if (fread(bar,RHIZOME_BAR_BYTES,1,f)<1) break;
    if (!bcmp(bid,&bar[RHIZOME_BAR_PREFIX_OFFSET],
	      RHIZOME_BAR_PREFIX_BYTES)) {
      if (rhizome_bar_version(bar)>=version) {
	if (config.debug.rhizome_async)
	  DEBUGF("RD Async channel#%d already knows about version %lld>=%lld of %016x* -- not queuing for announcement",
		 channelNumber,rhizome_bar_version(bar),version,rhizome_bar_bidprefix_ll(bar));
	return 1;
      }
      else return 0;
    }
  }
  fclose(f);
  return 0;
}

int rhizome_direct_async_write_state(int channelNumber)
{
  FILE *f;
  char filename[1024];
  snprintf(filename,1024,"%s/state",
	   config.rhizome.direct.channels.av[channelNumber].value.out_path);
  f=fopen(filename,"w");
  if (f) {
    fprintf(f,"%lld:%lld\n",  
	    channel_states[channelNumber].lastInsertionTime,
	    channel_states[channelNumber].lastTXMessageNumber);
    fclose(f);
    return 0;
  }
  return -1;
}

int rhizome_direct_async_queue_ihave(int channelNumber,
				     rhizome_manifest *m,
				     int64_t insertionTime)
{
  long long version=rhizome_manifest_get_ll(m, "version");
  
  // Ignore manifests that we know that the far end already has
  if (rhizome_direct_async_does_far_end_have(channelNumber,
					     m->cryptoSignPublic,version)) {
    if (config.debug.rhizome_async)
      DEBUGF("Not announcing %s.%lld to channel#%d, as they already have it",
	     alloca_tohex_bid(m->cryptoSignPublic),version,channelNumber);
    return 0;
  }
  if (config.debug.rhizome_async)
    DEBUGF("Queueing %s.%lld for announcement via channel#%d",
	   alloca_tohex_bid(m->cryptoSignPublic),version,channelNumber);
  char filename[1024];
  snprintf(filename,1024,"%s/queued_manifests",
	   config.rhizome.direct.channels.av[channelNumber].value.out_path);
  unsigned char bar[RHIZOME_BAR_BYTES];
  if (rhizome_manifest_to_bar(m,bar)) {
    if (config.debug.rhizome_async)
      DEBUGF("rhizome_manifest_to_bar() failed");
    return -1;
  }
  FILE *f=fopen(filename,"a");
  if (!f) {
    return WHYF("Could not open '%s' for append",filename);
  }
  if (fwrite(bar,RHIZOME_BAR_BYTES,1,f)!=1) {
    if (config.debug.rhizome_async)
      DEBUGF("Appending BAR to '%s' failed",filename);
  }
  fclose(f);

  // Now update channel state so that we don't requeue the same bundle
  if (config.debug.rhizome_async)
    DEBUGF("Setting lastInsertionTime of channel #%d to %lld",
	   channelNumber,insertionTime);
  channel_states[channelNumber].lastInsertionTime=insertionTime;
  rhizome_direct_async_write_state(channelNumber);
  rhizome_direct_async_load_state();
  return 0;
}

int rhizome_direct_sync_bundle_added(rhizome_manifest *m,int64_t insertionTime)
{
  if (config.debug.rhizome_async)
    DEBUGF("new manifest: BID=%s, insertTime is %s",
	   alloca_tohex_bid(m->cryptoSignPublic),
	   describe_time_interval_ms(gettime_ms()-insertionTime));
  int i;
  for(i=0;i<config.rhizome.direct.channels.ac;i++) {
    if (channel_states[i].lastInsertionTime<insertionTime) {
      if (config.debug.rhizome_async)
	DEBUGF("Bundle is new for channel #%d",i);
      rhizome_direct_async_queue_ihave(i,m,insertionTime);
    }
  }
  // Re-read async channel state, in case we have appended to any
  // queues.
  rhizome_direct_async_load_state();
  return 0;
}

static int messagesRequired(int bytesPerMessage,int bytesToSend)
{
  /* Overhead per message is as follows to deal with out-of-order delivery.
     We don't do retransmission or detection of dropped messages.     
     14 bits - sequence number (to help deal with out-of-order delivery)
     1 bit - communication start (to help get back in sync in case we do
     have some dropped messages)
     1 bit - communication end (to help get back in sync in case we do
     have some dropped messages)
  */
  int netBytesPerMessage=bytesPerMessage-1-1;
  DEBUGF("bytesPerMessage=%d (%d usable), bytesToSend=%d",
	 bytesPerMessage,netBytesPerMessage,bytesToSend);
  DEBUGF("estimated messages = %d",bytesToSend/netBytesPerMessage);

  return bytesToSend/netBytesPerMessage+(bytesToSend%netBytesPerMessage?1:0);
}

static int writeMessage(FILE *f,struct overlay_buffer *b,int m,int of,int channel)
{
  /* Construct message header */
  unsigned char header[2];
  int startP=(!m)?1:0;
  int endP=(m==(of-1))?1:0;
  unsigned short header_short=channel_states[channel].lastTXMessageNumber&0x3fff;
  header_short|=(startP<<14);
  header_short|=(endP<<15);
  header[0]=header_short>>8;
  header[1]=header_short&0xff;
  if (fwrite(&header[0],2,1,f)!=1) return -1;

  int bytes=config.rhizome.direct.channels.av[channel].value.message_length-2;
  switch (config.rhizome.direct.channels.av[channel].value.alphabet_size)
    {
    case 256: /* 8-bit clean */
      if (b->position<((config.rhizome.direct.channels.av[channel]
			.value.message_length-2)*m+bytes))
	bytes=b->position
	  -(config.rhizome.direct.channels.av[channel].value.message_length-2)*m;

      if (fwrite(&b->bytes[config.rhizome.direct.channels.av[channel]
			   .value.message_length*m],bytes,1,f)==1)
	return 0;
      else return -1;
      break;
    case 128: /* 7-bit clean */
    default: /* arbitrary alphabet, use range-coder */
      WHYF("Oops -- I don't support output in base %d yet.",
	   config.rhizome.direct.channels.av[channel].value.alphabet_size);
      return -1;
    }
}

int app_rhizome_direct_async_check(int argc, const char *const *argv, const struct command_line_option *o, void *context)
{
  struct pollfd fds[1];
  struct monitor_state *state;
  
  int monitor_client_fd = monitor_client_open(&state);
  
  write(monitor_client_fd, "rdasync check\n",strlen("rdasync check\n"));

  fds[0].fd = monitor_client_fd;
  fds[0].events = POLLIN;
  
  while(1){
    int r = poll(fds, 1, 100);
    if (r>0){
            
      if (fds[0].revents & POLLIN){
	char line[1024];
	read(monitor_client_fd,line,1024);
	if (strstr(line,"\nOK:\n")) {
	  INFOF("Serval process has accepted scan request.");
	  break;
	}
	DEBUGF("monitor interface says '%s'",line);
      }
      
      if (fds[0].revents & (POLLHUP | POLLERR))
	break;
    }
  }
  
  monitor_client_close(monitor_client_fd, state);
  monitor_client_fd=-1;
  
  return 0;
}


int rhizome_direct_async_flush_queue(int channel)
{
  int j;
  int messageBytes=config.rhizome.direct.channels.av[channel].value.message_length;
  int maxMessages=config.rhizome.direct.channels.av[channel].value.max_messages;
					 
  DEBUGF("Preparing upto %d messages of upto %d bytes,"
	 " for rhizome content arrived later than %s",
	 maxMessages,messageBytes,
	 describe_time_interval_ms(channel_states[channel].lastInsertionTime));

#define MAX_FRESH 128
  int freshBundles=0;
  unsigned char bars[MAX_FRESH][RHIZOME_BAR_BYTES]; // 128x32 = 4KB
  int manifestBytes[MAX_FRESH];
  unsigned long long insertTimes[MAX_FRESH];

  char filename[1024];
  snprintf(filename,1024,"%s/queued_manifests",
	   config.rhizome.direct.channels.av[channel].value.out_path);
  FILE *f=fopen(filename,"r");
  
  while (freshBundles<MAX_FRESH
	 &&(fread(bars[freshBundles],RHIZOME_BAR_BYTES,1,f)==1)) {

    rhizome_manifest *m=NULL;
    if (rhizome_retrieve_manifest_by_bar(bars[freshBundles],&m)!=1)
      {
	if (m) rhizome_manifest_free(m);
	if (config.debug.rhizome_async) {
	  DEBUGF("Failed to retrieve manifest for bar");
	  dump("BAR",bars[freshBundles],RHIZOME_BAR_BYTES);
	}
	continue;
      }
    manifestBytes[freshBundles]=m->manifest_all_bytes;

    freshBundles++;    
  }
  fclose(f);

  if (config.debug.rhizome_async) 
    DEBUGF("There are %d bundles that we need to tell the far side about.",
	   freshBundles);
  if (!freshBundles) {
    if (config.debug.rhizome_async) 
      DEBUG("There is nothing to do.");
    return 0;
  }

  uint64_t lastInsertTimeCovered=insertTimes[freshBundles-1];

  /* If there are very few fresh bundles, it is more efficient to just send the
     manifests, rather than go back and forth to see what they already have.     

     We only keep the latest version of any given manifest, so rather annoyingly
     we can't tell if the far end already knows about a manifest.  What we can do
     is send abbreviated BIDs and versions, which will allow the far end to
     work out with high probability (but not certainty) whether they have the
     bundle in question.
  */
  
  struct overlay_buffer *header=ob_new();
  struct overlay_buffer *announceAsBars=ob_new();
  struct overlay_buffer *announceAsManifests=ob_new();
  
  /* Start of time range */
  ob_append_ui32(header,channel_states[channel].firstManifestQueueTime>>32);
  ob_append_ui32(header,channel_states[channel].firstManifestQueueTime&0xffffffff);
  if (lastInsertTimeCovered-channel_states[channel].firstManifestQueueTime>0xffffffffLL)
    ob_append_ui32(header,0xffffffff);
  else
    ob_append_ui32(header,(unsigned int)(lastInsertTimeCovered
					 -channel_states[channel].firstManifestQueueTime));

  ob_append_bytes(announceAsBars,header->bytes,header->position);
  ob_append_byte(announceAsBars,RDA_MSG_BARS_RAW);
  for(j=0;j<freshBundles;j++) {
    /* XXX compact BARs (geobounding box and version are low entropy).
       Version is also potentially low entropy.  We can certainly order
       the BARs by version so that the entropy of the BARs is reduced. */
    ob_append_bytes(announceAsBars,bars[j],RHIZOME_BAR_BYTES);
  }

  ob_append_bytes(announceAsManifests,header->bytes,header->position);
  ob_append_byte(announceAsManifests,RDA_MSG_MANIFESTS);
  int k;
  for(k=0;k<freshBundles;k++) {    

    rhizome_manifest *m=NULL;
    if (rhizome_retrieve_manifest_by_bar(bars[k],&m)<1)
      {
	if (m) rhizome_manifest_free(m);
	continue;
      }

    if (manifestBytes[k]<250)
      ob_append_byte(announceAsManifests,manifestBytes[k]);
    else {
      ob_append_byte(announceAsManifests,250+manifestBytes[k]/250);
      ob_append_byte(announceAsManifests,manifestBytes[k]%255);
    }     
    ob_makespace(announceAsManifests,manifestBytes[k]);
    bcopy(m->manifestdata,
	  &announceAsManifests->bytes[announceAsManifests->position],
	  manifestBytes[k]);
    announceAsManifests->position+=manifestBytes[k];
    rhizome_manifest_free(m);
  }

  if (config.debug.rhizome_async) 
    {
      DEBUGF("Requires %d bytes (%d messages) as raw BARs",
	     announceAsBars->position,
	     messagesRequired(messageBytes,announceAsBars->position));
      DEBUGF("Requires %d bytes (%d messages) as raw Manifests",
	     announceAsManifests->position,
	     messagesRequired(messageBytes,announceAsManifests->position));
    }

  /* Work out best format.
     We want the fewest messages, but also the most information.
     So we compare on the number of messages required, not the exact number
     of bytes, because we might be able to use the spare bytes productively.
     Thus the order of these tests is with most preferable last, but trumping
     previous decisions.  */
  int bestMessagesRequired=messagesRequired(messageBytes,announceAsBars->position);
  struct overlay_buffer *bestbuffer=announceAsBars;
  if (messagesRequired(messageBytes,announceAsManifests->position)
      <=bestMessagesRequired) {
    bestMessagesRequired=messagesRequired(messageBytes,
					  announceAsManifests->position);
    bestbuffer=announceAsManifests;
  }
  
  if (config.debug.rhizome_async)
    {
      DEBUGF("Sending using %d bytes (%d messages)",
	     bestbuffer->position,bestMessagesRequired);
    }

  /* Write out the actual message files */
  for(j=0;j<bestMessagesRequired;j++) {
    snprintf(filename,1024,"%s/tx.%016llx",
	     config.rhizome.direct.channels.av[channel].value.out_path,
	     channel_states[channel].lastTXMessageNumber);
    f=fopen(filename,"w");
    if (f) {
      writeMessage(f,bestbuffer,j,bestMessagesRequired,channel);
      fclose(f);
      channel_states[channel].lastTXMessageNumber++;
      rhizome_direct_async_write_state(channel);
    } else {
      WHYF("Error writing rhizome direct async message file '%s'.  Expect strange behaviour and synchronisation failure.",filename);
    }
  }

  /* Remove list of queued manifests */
  snprintf(filename,1024,"%s/queued_manifests",
	   config.rhizome.direct.channels.av[channel].value.out_path);
  if (config.debug.rhizome_async) DEBUGF("unlink(%s)",filename);
  unlink(filename);

  if (config.rhizome.direct.channels.av[channel].value.push_command
      &&config.rhizome.direct.channels.av[channel].value.push_command[0]) {
    int r=system(config.rhizome.direct.channels.av[channel].value.push_command);
    if (config.debug.rhizome_async)
      DEBUGF("Ran push command (exit=%d): %s",r,
	     config.rhizome.direct.channels.av[channel].value.push_command);
  }

  /* Re-read state */
  rhizome_direct_async_load_state();

  /* Clean up data structures */
  ob_free(header);
  ob_free(announceAsManifests);
  ob_free(announceAsBars);

  return 0;
}
