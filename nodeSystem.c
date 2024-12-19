#include <nodeSystem.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <limits.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <time.h>

typedef struct{
	int sID;
	void* memory;
	char* pipeName;
	uint8_t count;
	uint8_t type;
	uint8_t unit;
	uint16_t length;
} _node_pipe;

static uint8_t _nodeSystemIsActive = 0;

static FILE* logFile;
static uint8_t no_log;
static time_t timeZone;
static uint16_t _pipe_count = 0;
static _node_pipe* _pipes = NULL;
static int _parent;

static const uint32_t _node_init_head = 0x83DFC690;
static const uint32_t _node_init_eof  = 0x85CBADEF;
static const uint32_t _node_begin_head = 0x9067F3A2;
static const uint32_t _node_begin_eof  = 0x910AC8BB;

static char* getRealTimeStr();

int nodeSystemInit(){
	//check system state
	if(_nodeSystemIsActive){
		return -3;
	}
	
	//set state
	_nodeSystemIsActive = 1;

	//set parentPid
	_parent = getppid();

	//calc timezone
	time_t t = time(NULL);
	struct tm lt = {0};
	localtime_r(&t, &lt);
	timeZone = lt.tm_gmtoff;

	//read no_log
	read(STDIN_FILENO,&no_log,sizeof(no_log));

	//read log file path
	uint16_t len;
	char tmp[PATH_MAX];
	read(STDIN_FILENO,&len,sizeof(len));
	read(STDIN_FILENO,tmp,len);

	//create log file
	if(!no_log){
		logFile = fopen(tmp,"w");
		if(logFile == NULL){
			no_log = 1;
		}
	}


	//send header
	write(STDOUT_FILENO,&_node_init_head,sizeof(_node_init_head));

	//send pipe count
	write(STDOUT_FILENO,&_pipe_count,sizeof(_pipe_count));

	//send pipe data
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		write(STDOUT_FILENO,&_pipes[i].type,sizeof(_pipes[i].type));
		write(STDOUT_FILENO,&_pipes[i].unit,sizeof(_pipes[i].unit));
		write(STDOUT_FILENO,&_pipes[i].length,sizeof(_pipes[i].length));
		write(STDOUT_FILENO,_pipes[i].pipeName,strlen(_pipes[i].pipeName)+1);
	}
	
	//send eof
	write(STDOUT_FILENO,&_node_init_eof,sizeof(_node_init_eof));
}

int nodeSystemAddPipe(char* const pipeName,NODE_PIPE_TYPE type,NODE_DATA_UNIT unit,uint16_t arrayLength){
	//check system state
	if(_nodeSystemIsActive){
		return -3;
	}

	//check pipe count
	if(_pipe_count == 0xFFFF){
		return -4;
	}
	
	//name check
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		if(strcmp(_pipes[i].pipeName,pipeName) == 0)
			return -5;
	}

	//malloc piep struct
	_node_pipe pipe = {};
	
	//cpy data
	pipe.type = type;
	pipe.unit = unit;
	pipe.length = arrayLength;
	pipe.pipeName = malloc(strlen(pipeName)+1);
	if(!pipe.pipeName){
		return -1;
	}
	strcpy(pipe.pipeName,pipeName);

	//add pipe array
	_node_pipe* tmp = _pipes;
	_pipes = realloc(_pipes,sizeof(_node_pipe)*(_pipe_count+1));
	if(!_pipes){
		_pipes = tmp;
		free(pipe.pipeName);
		return -2;
	}
	_pipes[_pipe_count] = pipe;

	return _pipe_count++;
}

int nodeSystemBegine(){
	//check system state
	if(_nodeSystemIsActive != 1){
		return -3;
	}

	//set state
	_nodeSystemIsActive = 2;

	//send header
	write(STDOUT_FILENO,&_node_begin_head,sizeof(_node_begin_head));

	//send pipe data
	uint16_t i;
	for(i = 0;i < _pipe_count;i++){
		if(_pipes[i].type != NODE_IN){
			read(STDIN_FILENO,&_pipes[i].sID,sizeof(_pipes[i].sID));
			if(_pipes[i].type == NODE_OUT)
				_pipes[i].memory = shmat(_pipes[i].sID,NULL,0);
			else
				_pipes[i].memory = shmat(_pipes[i].sID,NULL,SHM_RDONLY);

			if(_pipes[i].memory < 0){
				nodeSystemDebugLog(strerror(errno));
				exit(1);
			}
		}
	}
	
	//send eof
	write(STDOUT_FILENO,&_node_begin_eof,sizeof(_node_begin_eof));

	//set nonblocking
	fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) | O_NONBLOCK);
}


int nodeSystemLoop(){
	uint16_t  pipeId;
	
	if(read(STDIN_FILENO,&pipeId,sizeof(pipeId)) == sizeof(uint16_t)){
		//set blocking
		fcntl(STDIN_FILENO ,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) & (~O_NONBLOCK));

		if(_pipes[pipeId].sID != 0){
			if(shmdt(_pipes[pipeId].memory) < 0){
				char msg[4096];
				sprintf(msg,"Pipe[%s] failed shmdt():%s",_pipes[pipeId].pipeName,strerror(errno));
				nodeSystemDebugLog(msg);
			}
		}

		_pipes[pipeId].count = 0;
		read(STDIN_FILENO,&_pipes[pipeId].sID,sizeof(_pipes[pipeId].sID));
		if(_pipes[pipeId].sID != 0){			
			_pipes[pipeId].memory = shmat(_pipes[pipeId].sID,NULL,SHM_RDONLY);

			char msg[4096];
			sprintf(msg,"Pipe[%s] pipe connected",_pipes[pipeId].pipeName);
			nodeSystemDebugLog(msg);
		}else{
			char msg[4096];
			sprintf(msg,"Pipe[%s] pipe diss connect",_pipes[pipeId].pipeName);
			nodeSystemDebugLog(msg);
		}
		

		//set nonblocking
		fcntl(STDIN_FILENO,F_SETFL,fcntl(STDIN_FILENO ,F_GETFL) | O_NONBLOCK);
	}


	return kill(_parent,0);
}

void nodeSystemDebugLog(char* const str){
	if(no_log)
		return;

	char* time = getRealTimeStr();
	size_t len = strlen(time);
	fwrite(time,1,len,logFile);

	fwrite(":",1,1,logFile);

	len = strlen(str);
	fwrite(str,1,len,logFile);

	fwrite("\n",1,1,logFile);

	fflush(logFile);
}

static char* getRealTimeStr(){
	static char timeStr[64];
	static time_t befor;

	struct timespec spec;
    struct tm _tm;
	clock_gettime(CLOCK_REALTIME_COARSE, &spec);
	spec.tv_sec += timeZone;

	if(spec.tv_sec == befor)
		return timeStr;

	gmtime_r(&spec.tv_sec,&_tm);
	strftime(timeStr,sizeof(timeStr),"%Y-%m-%d-(%a)-%H:%M:%S",&_tm);

	befor = spec.tv_sec;
	return timeStr;
}

int nodeSystemRead(int pipeID,void* buffer,uint16_t size){
	
	if(!_pipes[pipeID].sID || _pipes[pipeID].type == NODE_OUT)
		return -1;
	
	//read count
	uint8_t count = ((char*)_pipes[pipeID].memory)[0];
	if(count == _pipes[pipeID].count)
		return 0;
	_pipes[pipeID].count = count;

	//copy data
	memcpy(buffer,_pipes[pipeID].memory+1,NODE_DATA_UNIT_SIZE[_pipes[pipeID].unit] * _pipes[pipeID].length);

	return 1;
}

int nodeSystemWrite(int pipeID,void* buffer,uint16_t size){

	if(_pipes[pipeID].type != NODE_IN)
		return -1;

	//write count
	((uint8_t*)_pipes[pipeID].memory)[0] = ++_pipes[pipeID].count;

	//copy data
	memcpy(_pipes[pipeID].memory+1,buffer,NODE_DATA_UNIT_SIZE[_pipes[pipeID].unit] * _pipes[pipeID].length);

	return 0;
}