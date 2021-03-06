#include "stdafx.h"
#include <iostream>

#include "tensorflow/cc/ops/const_op.h"
#include "tensorflow/cc/ops/image_ops.h"
#include "tensorflow/cc/ops/standard_ops.h"
#include "tensorflow/core/framework/graph.pb.h"
#include "tensorflow/core/framework/tensor.h"
#include "tensorflow/core/graph/default_device.h"
#include "tensorflow/core/graph/graph_def_builder.h"
#include "tensorflow/core/lib/core/stringpiece.h"
#include "tensorflow/core/lib/core/threadpool.h"
#include "tensorflow/core/lib/io/path.h"
#include "tensorflow/core/lib/strings/stringprintf.h"
#include "tensorflow/core/platform/init_main.h"
#include "tensorflow/core/platform/types.h"
#include "tensorflow/core/public/session.h"
#include "tensorflow/core/util/command_line_flags.h"
#include "tensorflow/core/platform/logging.h"

using namespace std;
using namespace tensorflow;

Status LoadGraph(const string& graph_file_name,
	std::unique_ptr<tensorflow::Session>* session) {
	// graph的定义
	tensorflow::GraphDef graph_def;
	Status load_graph_status =
		ReadBinaryProto(tensorflow::Env::Default(), graph_file_name, &graph_def);

	/*
	int node_count = graph_def.node_size();
	for (int i = 0; i < node_count; i++) {
		auto n = graph_def.node(i);
		cout << "Names: " << n.name() << endl;
	}*/

	if (!load_graph_status.ok()) {
		return tensorflow::errors::NotFound("Failed to load compute graph at '",
			graph_file_name, "'");
	}

	session->reset(tensorflow::NewSession(tensorflow::SessionOptions()));

	Status session_create_status = (*session)->Create(graph_def);
	if (!session_create_status.ok()) {
		return session_create_status;
	}

	return Status::OK();
}

Status ReadTensorFromImageFile(const string& file_name, const int input_height,
	const int input_width, std::vector<Tensor>* out_tensors) {
	auto root = tensorflow::Scope::NewRootScope();
	using namespace ::tensorflow::ops;	//NOTINT(build/namespace)

	string input_name = "file_reader";
	string output_name = "img_tensor";
	auto file_reader = 
		tensorflow::ops::ReadFile(root.WithOpName(input_name), file_name);

	// Now try to figure out what kind of file it is and decode it.
	const int wanted_channels = 3;
	tensorflow::Output image_reader;
	// JPEG reader
	image_reader = DecodeJpeg(root.WithOpName("jpeg_reader"), file_reader,
		DecodeJpeg::Channels(wanted_channels));

	// Now cast the image data to float so we can do normal match on it.
	auto ufloat_caster =
		Cast(root.WithOpName("ufloat_caster"), image_reader, tensorflow::DT_FLOAT);

	auto normalization =
		Div(root.WithOpName("normalization"), ufloat_caster, float(255.0));

	// The convention for image ops in TensorFlow is that all images are expected
	// to be in batches, so that they're four-dimensional arrays with indices of
	// [batch, height, width, channel]. Because we only have a single image, we
	// have to add a batch dimension of 1 to the start with ExpandDims().
	auto dims_expander = ExpandDims(root.WithOpName(output_name), normalization, 0);

	// This runs the GraphDef network definition that we've just contructed, and
	// returns the results in the ouput tensor
	tensorflow::GraphDef graph;
	TF_RETURN_IF_ERROR(root.ToGraphDef(&graph));

	std::unique_ptr<tensorflow::Session> session(
		tensorflow::NewSession(tensorflow::SessionOptions()));
	TF_RETURN_IF_ERROR(session->Create(graph));
	TF_RETURN_IF_ERROR(session->Run({}, { output_name }, {}, out_tensors));
	return Status::OK();
}

Status SaveTensorToImageFile(Tensor img_tensor, std::vector<Tensor>* outputs) {
	auto root = tensorflow::Scope::NewRootScope();
	using namespace ::tensorflow::ops;	//NOTINT(build/namespace)

	string squeeze_name = "squeeze";
	string multiply_name = "multiply";
	string save_tensor_name = "save_tensor";
	string write_name = "write_file";

	auto squeeze = Squeeze(root.WithOpName(squeeze_name), img_tensor);
	auto multiply_255 = Multiply(root.WithOpName(multiply_name), squeeze, float(255.0));

	// Now cast the image data to float so we can do normal match on it.
	auto uint_caster =
		Cast(root.WithOpName("ufloat_caster"), multiply_255, tensorflow::DT_UINT8);
	auto dims_expander = ExpandDims(root.WithOpName("expand_dims"), uint_caster, -1);
	tensorflow::Output img_saver;
	img_saver = EncodeJpeg(root.WithOpName(save_tensor_name), dims_expander);
	
	string write_file_name = "./data/write_file.jpg";
	tensorflow::ops::WriteFile(root.WithOpName(write_name), write_file_name, img_saver);
	// This runs the GraphDef network definition that we've just contructed, and
	// returns the results in the ouput tensor
	tensorflow::GraphDef graph;
	TF_RETURN_IF_ERROR(root.ToGraphDef(&graph));

	std::unique_ptr<tensorflow::Session> session(
		tensorflow::NewSession(tensorflow::SessionOptions()));
	TF_RETURN_IF_ERROR(session->Create(graph));
	
	TF_RETURN_IF_ERROR(session->Run({}, { }, { write_name }, outputs));
	return Status::OK();
}

int main()
{
	string image = "data/1_0.jpg";
	string image_name = "1_0.jpg";
	string graph = "./model/frozen_model.pb";	// path to freezen model
	INT32 input_width = 1024;
	INT32 input_height = 1024;
	string input = "input_x:0";
	string output = "metrics/Sigmoid:0";
	
	// Load and initialize the model
	std::unique_ptr<tensorflow::Session> session;
	string graph_path = graph;
	Status load_graph_status = LoadGraph(graph_path, &session);
	if (!load_graph_status.ok()) {
		LOG(ERROR) << load_graph_status;
		return -1;
	}
	else {
		cout << "load graph ok!" << endl;
	}

	// Get the image from disk as a float array of numbers
	string image_path = image;
	std::vector<Tensor> image_tensor;
	tensorflow::Tensor image_size_tensor(tensorflow::DT_INT32, tensorflow::TensorShape({ 3 }));
	image_size_tensor.vec<int32>()(0) = input_width;
	image_size_tensor.vec<int32>()(1) = input_height;

	Status read_tensor_status = ReadTensorFromImageFile(image_path, input_height, input_width, &image_tensor);
	if (!read_tensor_status.ok()) {
		LOG(ERROR) << read_tensor_status;
		return -1;
	}


	// Actually run the image through the model
	std::vector<Tensor> outputs;
	const Tensor& input_tensor = image_tensor[0];
	// cout << input_tensor.shape() << endl;
	
	/*
	Status run_model_status = RunModel(&session, input, input_tensor,
	{ output_num_detections, output_detection_boxes,output_detection_scores, output_detection_classes },
	&outputs)
	*/
	/*
	FILE* stream;
	freopen_s(&stream, "/dev/null", "w", stdout);
	*/

	cout << "session begin..." << endl;
	auto status = session->Run({ {input, input_tensor} }, { output }, {}, &outputs);
	cout << "session finished..." << endl;
	cout << status.ToString() << endl;
	
	Tensor output_img = outputs[0];
	std::vector<Tensor> output_image_tensor;
	Status save_status = SaveTensorToImageFile(output_img, &output_image_tensor);
	if (!save_status.ok()) {
		LOG(ERROR) << save_status;
		system("pause");
		return -1;
	}
	cout << save_status.ToString() << endl;

	system("pause");

	return 0;
}