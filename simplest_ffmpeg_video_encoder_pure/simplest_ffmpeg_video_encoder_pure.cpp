/**
 * 最简单的基于FFmpeg的视频编码器（纯净版）
 * Simplest FFmpeg Video Encoder Pure
 * 
 * 雷霄骅 Lei Xiaohua
 * leixiaohua1020@126.com
 * 中国传媒大学/数字电视技术
 * Communication University of China / Digital TV Technology
 * http://blog.csdn.net/leixiaohua1020
 * 
 * 本程序实现了YUV像素数据编码为视频码流（H264，MPEG2，VP8等等）。
 * 它仅仅使用了libavcodec（而没有使用libavformat）。
 * 是最简单的FFmpeg视频编码方面的教程。
 * 通过学习本例子可以了解FFmpeg的编码流程。
 * This software encode YUV420P data to video bitstream
 * (Such as H.264, H.265, VP8, MPEG2 etc).
 * It only uses libavcodec to encode video (without libavformat)
 * It's the simplest video encoding software based on FFmpeg. 
 * Suitable for beginner of FFmpeg 
 */


#include <stdio.h>
#include <omp.h>
#include <vector>

#include <mutex>
#include <condition_variable>
#include <deque>

#define __STDC_CONSTANT_MACROS

#ifdef _WIN32
//Windows
extern "C"
{
#include "libavutil/opt.h"
#include "libavcodec/avcodec.h"
#include "libavutil/imgutils.h"
};
#else
//Linux...
#ifdef __cplusplus
extern "C"
{
#endif
#include <libavutil/opt.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#ifdef __cplusplus
};
#endif
#endif

//test different codec
#define TEST_H264  1
#define TEST_HEVC  0

//Source: (Slightly modified)
//https://stackoverflow.com/questions/12805041/c-equivalent-to-javas-blockingqueue
template <typename T>
class queue
{
private:
    std::mutex              d_mutex;
    std::condition_variable d_condition;
    std::deque<T>           d_queue;
public:
    //void push(T const& value) {
    void push(T value) {
        {
            std::unique_lock<std::mutex> lock(this->d_mutex);
            d_queue.push_front(value);
        }
        this->d_condition.notify_one();
    }
    T pop() {
        std::unique_lock<std::mutex> lock(this->d_mutex);
        this->d_condition.wait(lock, [=]{ return !this->d_queue.empty(); });
        T rc(std::move(this->d_queue.back()));
        this->d_queue.pop_back();
        return rc;
    }
    bool empty(){
	    return this->d_queue.empty();
    }
};


int main(int argc, char* argv[])
{
	AVCodec *pCodec;
    AVCodecContext *pCodecCtx= NULL;
    int i, ret, got_output;
    FILE *fp_in;
	FILE *fp_out;
    AVFrame *pFrame;
    AVPacket pkt;
	int y_size;
	int framecnt=0;


	/*
	 * Files and there WxH used for testing
	 */
	//char filename_in[]="../ds_480x272.yuv";
	//char filename_in[]="../640x360.yuv";
	//char filename_in[]="../960x540.yuv";
	char filename_in[]="../1280x720.yuv";

#if TEST_HEVC
	AVCodecID codec_id=AV_CODEC_ID_HEVC;
	char filename_out[]="ds.hevc";
#else
	AVCodecID codec_id=AV_CODEC_ID_H264;

	/*
	 * Output file names
	 */
	//char filename_out[]="ds.h264";
	//char filename_out[]="640x360.h264";
	//char filename_out[]="960x540.h264";
	char filename_out[]="1280x720.h264";
#endif

	/*
	 * Parameters used for each file to be encoded
	 */

	//int in_w=480,in_h=272;	
	//int framenum=100;	
	
	//int in_w=640,in_h=360;	
	//int framenum=400;	

	//int in_w=960,in_h=540;	
	//int framenum=400;	

	int in_w=1280,in_h=720;	
	int framenum=677;	

	avcodec_register_all();

    pCodec = avcodec_find_encoder(codec_id);
    if (!pCodec) {
        printf("Codec not found\n");
        return -1;
    }
    pCodecCtx = avcodec_alloc_context3(pCodec);
    if (!pCodecCtx) {
        printf("Could not allocate video codec context\n");
        return -1;
    }
    pCodecCtx->bit_rate = 400000;
    pCodecCtx->width = in_w;
    pCodecCtx->height = in_h;
    pCodecCtx->time_base.num=1;
	pCodecCtx->time_base.den=25;
    pCodecCtx->gop_size = 10;
    pCodecCtx->max_b_frames = 1;
    pCodecCtx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (codec_id == AV_CODEC_ID_H264)
        av_opt_set(pCodecCtx->priv_data, "preset", "slow", 0);
 
    if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
        printf("Could not open codec\n");
        return -1;
    }
    
    pFrame = av_frame_alloc();
    if (!pFrame) {
        printf("Could not allocate video frame\n");
        return -1;
    }
    pFrame->format = pCodecCtx->pix_fmt;
    pFrame->width  = pCodecCtx->width;
    pFrame->height = pCodecCtx->height;

    ret = av_image_alloc(pFrame->data, pFrame->linesize, pCodecCtx->width, pCodecCtx->height,
                         pCodecCtx->pix_fmt, 16);
    if (ret < 0) {
        printf("Could not allocate raw picture buffer\n");
        return -1;
    }

	//Input raw data
	fp_in = fopen(filename_in, "rb");
	if (!fp_in) {
		printf("Could not open %s\n", filename_in);
		return -1;
	}
	//Output bitstream
	fp_out = fopen(filename_out, "wb");
	if (!fp_out) {
		printf("Could not open %s\n", filename_out);
		return -1;
	}

	y_size = pCodecCtx->width * pCodecCtx->height;
   
   /*
    * Enums for holding status information
    */
   enum ExitFlag {RETURN, BREAK, NONE};
   enum ReadFlag {STILL_READING, DONE_READING};
   enum EncodeFlag {STILL_ENCODING, DONE_ENCODING};
   ExitFlag exit_flag = NONE;
   ReadFlag read_flag = STILL_READING;
   EncodeFlag encode_flag = STILL_ENCODING;

   /*
    * Queues used for producing and consuming threads
    */
   queue<AVFrame*> encodeQ;
   queue<AVPacket*> writeQ;


/*
 * Our parallelized section
 *
 * 	Each section within the pragma omp parallel sections
 * 	is given its own thread to perform the enclosed code.
 * 	Blocking queues are used to facilitate a producer
 * 	consumer style workflow so each thread is waiting to
 * 	perform their tasks as little as possible
 *
 * 	exit_flag: used to emulate breaks and returns since
 * 		omp does not work with breaks and returns
 *
 * 	Reading thread: This thread will read in all available
 * 		data frames from the input file and push them
 * 		onto a blocking queue
 *
 * 	Encoding thread: This thread will consume from the blocking
 * 		read queue and encode each frame of data that it gets.
 * 		Encoded data is then pushed onto the writing queue to
 * 		be written later
 *
 * 	Writing thread: This thread will consume from the writing queue
 * 		and will write each frame of encoded data to the final
 * 		output file
 *
 *
 */
#pragma omp parallel sections
   {
     #pragma omp section
     {
	   // Reading Thread
	   printf("Starting Reading\nframenum: %d\n", framenum);
	   AVFrame *tempFrame;
	   for (i = 0; i < framenum; i++) {
	       //allocate new frame to read input to
	       if(exit_flag == NONE){
		   tempFrame = av_frame_alloc();
		   if (!tempFrame) {
		       printf("Could not allocate video frame\n");
		       exit_flag = RETURN;
		   }
	       }
	       if(exit_flag == NONE){
		   tempFrame->format = pCodecCtx->pix_fmt;
		   tempFrame->width  = pCodecCtx->width;
		   tempFrame->height = pCodecCtx->height;

		   ret = av_image_alloc(tempFrame->data, tempFrame->linesize, pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, 16);
		   if (ret < 0) {
		       printf("Could not allocate raw picture buffer\n");
		       exit_flag = RETURN;
		   }
	       }
	       if (exit_flag == NONE) {
		   //Read raw YUV data from fp_in into pFrame->data
		   if (fread(tempFrame->data[0], 1, y_size, fp_in) <= 0 ||      // Y
		       fread(tempFrame->data[1], 1, y_size / 4, fp_in) <= 0 ||	// U
		       fread(tempFrame->data[2], 1, y_size / 4, fp_in) <= 0) {	// V
		       exit_flag = RETURN;
		   }
		   else if (feof(fp_in)) {
		       exit_flag = BREAK;
		   }

		   if (exit_flag == NONE) {
		       tempFrame->pts = i;
		       encodeQ.push(tempFrame);
		   }
	       }
	   }
	   read_flag = DONE_READING; // finished with reading input
     }   
     #pragma omp section
     {
	   // Encoding Threads
	   printf("Starting Encoding\n");

	   AVFrame *tempEncodeFrame;
	   AVPacket *tempPkt;
	   while(read_flag != DONE_READING || !encodeQ.empty()){
	       tempPkt = new AVPacket;
	       av_init_packet(tempPkt);
	       tempPkt->data = NULL;    // packet data will be allocated by the encoder
	       tempPkt->size = 0;
	       if (exit_flag == NONE) {
		   tempEncodeFrame = encodeQ.pop();
		   ret = avcodec_encode_video2(pCodecCtx, tempPkt, tempEncodeFrame, &got_output);

		   if (ret < 0) {
		       printf("Error encoding frame\n");
		       exit_flag = BREAK;
		   }
		   if (got_output) {
		       writeQ.push(tempPkt);
		   }
	       }
	   }
	   encode_flag = DONE_ENCODING; // finished encoding process
     }
     #pragma omp section
     {
	   // Writing Threads
	   printf("Starting Writing\n");

	   AVPacket *tempWritePkt;
	   i = 0;
	   while (encode_flag != DONE_ENCODING || !writeQ.empty()) {
	       if (exit_flag == NONE) {
		   //pkt = toWrite[i];
		   tempWritePkt = writeQ.pop();
		   printf("Succeed to encode frame: %5d\tsize:%5d\n", i++, tempWritePkt->size);
		   fwrite(tempWritePkt->data, 1, tempWritePkt->size, fp_out);
	       }
	   }
     }

   }
   if (exit_flag == RETURN) { return -1; }

    //Init packet for flush encoder to flush data to
    av_init_packet(&pkt);
    pkt.data = NULL;    
    pkt.size = 0;
    //Flush Encoder
    for (int got_output = 1; got_output; i++) {
        //TODO: Getting fault thrown on pkt[buf] (pkt's buf variable).
        ret = avcodec_encode_video2(pCodecCtx, &pkt, NULL, &got_output);
        if (ret < 0) {
            printf("Error encoding frame\n");
            return -1;
        }
        if (got_output) {
            printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n", pkt.size);
            fwrite(pkt.data, 1, pkt.size, fp_out);
            av_free_packet(&pkt);
        }
    }

    fclose(fp_out);
    avcodec_close(pCodecCtx);
    av_free(pCodecCtx);
    av_freep(&pFrame->data[0]);
    //av_frame_free(&pFrame);

	return 0;
}

