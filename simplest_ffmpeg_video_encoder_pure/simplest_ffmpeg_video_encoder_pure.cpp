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


void thread_write(omp_lock_t* write_lock, const void *data, size_t size, size_t nitems, FILE *fp_out)
{
   omp_set_lock(write_lock);
   fwrite(data, nitems, size, fp_out);
   omp_unset_lock(write_lock);
}


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

   //OpenMP I/O locks
   omp_lock_t read_lock;
   omp_lock_t write_lock;
   omp_init_lock(&read_lock);
   omp_init_lock(&write_lock);

	char filename_in[]="../ds_480x272.yuv";

#if TEST_HEVC
	AVCodecID codec_id=AV_CODEC_ID_HEVC;
	char filename_out[]="ds.hevc";
#else
	AVCodecID codec_id=AV_CODEC_ID_H264;
	char filename_out[]="ds.h264";
#endif


	int in_w=480,in_h=272;	
	int framenum=100;	

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
   
   //enums for holding status information
   enum ExitFlag {RETURN, BREAK, NONE};
   enum InitFlag {INIT, INIT_DONE};
   ExitFlag exit_flag = NONE;



   // Reading Threads
   printf("Starting Reading\nframenum: %d\n", framenum);
   std::vector<AVFrame> toEncode;
   for (i = 0; i < framenum; i++) {
       if (exit_flag == NONE) {
           //Read raw YUV data from fp_in into pFrame->data
           if (fread(pFrame->data[0], 1, y_size, fp_in) <= 0 ||		// Y
               fread(pFrame->data[1], 1, y_size / 4, fp_in) <= 0 ||	// U
               fread(pFrame->data[2], 1, y_size / 4, fp_in) <= 0) {	// V
               //return -1;
               exit_flag = RETURN;
           }
           else if (feof(fp_in)) {
               //break;
               exit_flag = BREAK;
           }

           if (exit_flag == NONE) {
               pFrame->pts = i;
               toEncode.push_back(*pFrame);
           }
       }
   }
   if (exit_flag == RETURN) { return -1; }

   // Encoding Threads
   printf("Starting Encoding\ntoEncode: %d\n", toEncode.size());



   std::vector<AVPacket> toWrite;
   for (i = 0; i < toEncode.size(); i++) {
       av_init_packet(&pkt);
       pkt.data = NULL;    // packet data will be allocated by the encoder
       pkt.size = 0;
       if (exit_flag == NONE) {
           pFrame = &toEncode[i];
           ret = avcodec_encode_video2(pCodecCtx, &pkt, pFrame, &got_output);

           if (ret < 0) {
               printf("Error encoding frame\n");
               //return -1;
               exit_flag = BREAK;
           }
           if (got_output) {
               toWrite.push_back(pkt);
           }
       }
   }
   if (exit_flag == RETURN) { return -1; }

   // Writing Threads
   printf("Starting Writing\n");

   for (i = 0; i < toWrite.size(); i++) {
       if (exit_flag == NONE) {
           pkt = toWrite[i];
           printf("Succeed to encode frame: %5d\tsize:%5d\n", i, pkt.size);
           fwrite(pkt.data, 1, pkt.size, fp_out);
       }
   }
   if (exit_flag == RETURN) { return -1; }


   av_free_packet(&pkt);


   /*
    //Encode
    #pragma omp parallel for private(ret, got_output, pkt, pFrame) firstprivate(init_flag)
    for (i = 0; i < framenum; i++) {
        /* 
        * OMP requires that for loops have the conditional format of "variable relational_operator variable",
        *  so we can't check this flag in the condition. Instead, I just check it here.
        
        if (exit_flag == NONE) {
            
            av_init_packet(&pkt);
            pkt.data = NULL;    // packet data will be allocated by the encoder
            pkt.size = 0;

            //Read raw YUV data from fp_in into pFrame->data
            omp_set_lock(&read_lock);
            if (fread(pFrame->data[0], 1, y_size, fp_in) <= 0 ||		// Y
                fread(pFrame->data[1], 1, y_size / 4, fp_in) <= 0 ||	// U
                fread(pFrame->data[2], 1, y_size / 4, fp_in) <= 0) {	// V
                //return -1;
                exit_flag = RETURN;
            }
            else if (feof(fp_in)) {
                //break;
                exit_flag = BREAK;
            }
            omp_unset_lock(&read_lock);

            pFrame->pts = i;    // set the timestamp for the frame
            /* encode the image */
            // encode pFrame and write the output to pkt
            /* Accoring to avcodec, "The output packet does not necessarily need to contain data for
            * the most recent frame, as encoders can delay and reorder input frames internally as needed.
            * So I think that as long as we set pframe->pts correctly above, we should be good to go for this
            * sources:
            *   - https://github.com/Nevcairiel/FFmpeg/blob/master/libavcodec/avcodec.h
            *   - https://github.com/Nevcairiel/FFmpeg/blob/master/libavutil/frame.h
            
            if (exit_flag == NONE) {
                ret = avcodec_encode_video2(pCodecCtx, &pkt, pFrame, &got_output);
            }
            if (exit_flag == NONE && ret < 0) {
                printf("Error encoding frame\n");
                //return -1;
                exit_flag = BREAK;
            }
            if (exit_flag == NONE && got_output) {
                // Question: why framecnt and not just i?
                //printf("Succeed to encode frame: %5d\tsize:%5d\n",framecnt,pkt.size);
                framecnt++;
                printf("Succeed to encode frame: %5d\tsize:%5d\n", i, pkt.size);
                fwrite(pkt.data, 1, pkt.size, fp_out);
                av_free_packet(&pkt);
            }
        }
    }*/

    //Flush Encoder
    for (int got_output = 1; got_output; i++) {
        //TODO: Getting fault thrown on pkt[buf] (pkt's buf variable).
        ret = avcodec_encode_video2(pCodecCtx, &pkt, NULL, &got_output);
        if (ret < 0) {
            printf("Error encoding frame\n");
            return -1;
        }
        if (got_output) {
            printf("Flush Encoder: Succeed to encode 1 frame!\tsize:%5d\n",pkt.size);
            fwrite(pkt.data, 1, pkt.size, fp_out);
            av_free_packet(&pkt);
        }
    }

    fclose(fp_out);
    avcodec_close(pCodecCtx);
    av_free(pCodecCtx);
    av_freep(&pFrame->data[0]);
    av_frame_free(&pFrame);

	return 0;
}

