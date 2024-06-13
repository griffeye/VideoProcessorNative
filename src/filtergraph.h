#ifndef LIBVX_FILTERGRAPH_H
#define LIBVX_FILTERGRAPH_H

#ifdef __cplusplus
extern "C" {
#endif

	vx_error vx_filtergraph_init(struct AVFilterGraph** filter_graph, enum AVMediaType type, const char* args);
	vx_error vx_filtergraph_configure(struct AVFilterGraph** filter_graph, enum AVMediaType type, struct AVFilterContext** last_filter, int* pad_index);
	vx_error vx_filtergraph_insert_filter(struct AVFilterContext** last_filter, int* pad_index, const char* filter_name, const char* filter_label, const char* args);
	vx_error vx_filtergraph_process_frame(struct AVFilterGraph** filter_graph, AVFrame* av_frame);

#ifdef __cplusplus
}
#endif

#endif
