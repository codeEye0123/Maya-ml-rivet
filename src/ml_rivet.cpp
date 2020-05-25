/*MIT License

Copyright (c) 2020 Mauro Lopez

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include "ml_rivet.h"
#include "readZip.h"


MTypeId     mlRivet::id(0x81212);

MObject		mlRivet::deviceType;
MObject		mlRivet::modelFilePath;
MObject		mlRivet::inDataFilePath;
MObject		mlRivet::inputs;

MObject		mlRivet::matrix;
MObject		mlRivet::outputs;

bool	mlRivet::_debug;

mlRivet::mlRivet() {}
mlRivet::~mlRivet() {}


std::vector<float> flattenMatrix(MMatrix inMatrix)
{
	std::vector<float> result;
	for (unsigned int i = 0; i < 4; ++i)
	{
		for (unsigned int j = 0; j < 4; ++j)
		{
			result.push_back(inMatrix[i][j]);
		}
	}
	return result;
}
//Normalize features by given mean and standard deviations.
std::valarray<float> featNorm(std::valarray<float>  features, std::valarray<float> mean, std::valarray<float> std)
{
	float epsilon = 1.19209e-07;
	std::valarray<float> feats_norm = (features - mean) / (std + epsilon);
	return feats_norm;
}

//Denormalize features by mean and standard deviation.
std::valarray<float> featDenorm(std::valarray<float>  features_norm, std::valarray<float> mean, std::valarray<float> std)
{
	std::valarray<float> features = (features_norm * std) + mean;
	return features;
}
MMatrix getMatrixFromTriangle(MVector vectorA, MVector vectorB, MVector vectorC, MVector &prevAim)
{
	MVector vx = vectorA + MVector(1, 0, 0);
	MVector vy = vectorB + MVector(0, 1, 0);
	MVector vz = vectorC + MVector(0, 0, 1);
	MVector ba = vy - vx;
	MVector ca = vz - vx;
	MVector normal_vector = (ca.normal() ^ ba.normal()).normal();
	MVector tangent_vector = (ba.normal() ^ normal_vector).normal();
	if (tangent_vector*prevAim < 0) {
		tangent_vector *= -1;
	}
	prevAim = tangent_vector;
	MVector cross_vector = (tangent_vector ^ normal_vector).normal();
	MVector position = vectorA;
	MMatrix matrix;
	matrix[0][0] = tangent_vector[0]; matrix[0][1] = tangent_vector[1]; matrix[0][2] = tangent_vector[2]; 
	matrix[1][0] = normal_vector[0];  matrix[1][1] = normal_vector[1];  matrix[1][2] = normal_vector[2];
	matrix[2][0] = cross_vector[0];   matrix[2][1] = cross_vector[1];   matrix[2][2] = cross_vector[2];
	matrix[3][0] = position[0];       matrix[3][1] = position[1];       matrix[3][2] = position[2];
	return matrix;
}
MStatus mlRivet::compute(const MPlug& plug, MDataBlock& data)
{
	MStatus stat;
	_debug = false;
	MDataHandle deviceType_h = data.inputValue(deviceType, &stat);
	MDataHandle modelFilePath_h = data.inputValue(modelFilePath, &stat);
	MDataHandle inDataFilePath_h = data.inputValue(inDataFilePath, &stat);
		MArrayDataHandle inputs_h = data.inputArrayValue(inputs);
	unsigned int numElements = inputs_h.elementCount();
	if (numElements<=0)
		return MS::kSuccess;

	std::wstring modelPath = modelFilePath_h.asString().asWChar();
	std::wstring inDataPath = inDataFilePath_h.asString().asWChar();
	
	
	std::map<std::string, std::valarray<float>> inDataMap = getDataMap(inDataPath);
	MMatrix matrix;
	std::vector<float> rawInputX;
	std::vector<float> rawMtrx;
	for (unsigned int i = 0; i < numElements; ++i)
	{
		inputs_h.jumpToArrayElement(i);
		matrix  = inputs_h.inputValue().asMatrix();
		rawMtrx = flattenMatrix(matrix);
		rawInputX.reserve(rawInputX.size() + distance(rawMtrx.begin(), rawMtrx.end()));
		rawInputX.insert(rawInputX.end(), rawMtrx.begin(), rawMtrx.end());
	}
	std::valarray<float> rawInput(rawInputX.data(), rawInputX.size());

	if (_debug)
	{
		std::map<std::string, std::valarray<float>>::iterator it = inDataMap.begin();

		// Iterate over the map using Iterator till end.
		while (it != inDataMap.end())
		{
			// Accessing KEY from element pointed by it.
			std::string name = it->first;
			// Accessing VALUE from element pointed by it.
			std::valarray<float> value = it->second;
			printVector(name, value);
			// Increment the Iterator to point to next entry
			it++;
		}
		printVector("rawInput", rawInput);

	}
	if (rawInput.size() != inDataMap[INMEAN].size()) {
		cout << "Error: input size is " << rawInput.size() << " and mean size is "<< inDataMap[INMEAN].size()  << endl;
		return MS::kFailure;
	}
	std::valarray<float> normalInput = featNorm(rawInput, inDataMap[INMEAN], inDataMap[INSTD]);

	if (_debug)
	{
		printVector("normalInput", normalInput);
	}

	int deviceTypeInt = deviceType_h.asInt();
	const CNTK::DeviceDescriptor device = CNTK::DeviceDescriptor::GPUDevice(0);
	if (deviceTypeInt == 1) {
		const CNTK::DeviceDescriptor device = CNTK::DeviceDescriptor::CPUDevice();
	}
	CNTK::FunctionPtr modelFunc = getModel(modelPath);
	
	if (modelFunc == NULL) {
		cout << "Unable to read Model" << endl;
		return MS::kFailure;
	}
	CNTK::Variable inputVar = modelFunc->Arguments()[0];
	CNTK::Variable outputVar = modelFunc->Output();
	// cast normalInput valarray<float> into vector<float>
	std::vector<float> normalInputX;
	normalInputX.assign(std::begin(normalInput), std::end(normalInput));

	// Create input value and input data map
	CNTK::ValuePtr inputVal = CNTK::Value::CreateBatch(inputVar.Shape(), normalInputX, device);
	std::unordered_map<CNTK::Variable, CNTK::ValuePtr> inputDataMap = { { inputVar, inputVal } };

	// Create output data map. Using null as Value to indicate using system allocated memory.
	// Alternatively, create a Value object and add it to the data map.
	std::unordered_map<CNTK::Variable, CNTK::ValuePtr> outputDataMap = { { outputVar, nullptr } };

	// Start evaluation on the device
	modelFunc->Evaluate(inputDataMap, outputDataMap, device);

	// Get evaluate result as dense output
	CNTK::ValuePtr outputVal = outputDataMap[outputVar];
	std::vector<std::vector<float>> outputData;
	outputVal->CopyVariableValueTo(outputVar, outputData);
	std::valarray<float> rawOutput(outputData[0].data(), outputData[0].size());
	std::valarray<float> prediction = featDenorm(rawOutput, inDataMap[OUTMEAN], inDataMap[OUTSTD]);
	if (_debug)
	{
		printVector("prediction", prediction);
	}
	int matrixNum = prediction.size() / 9;
	MArrayDataHandle outputs_h = data.outputArrayValue(outputs);
	MArrayDataBuilder builder = outputs_h.builder(&stat);
	MMatrix outMatrix;
	if (prevAim.size() <= 1) {
		MVector aim;
		prevAim.clear();
	for (int i = 0; i < matrixNum; i++) {
			prevAim.push_back(aim);
		}
	}
	int j;
	for (int i = 0; i < matrixNum; i++) {
		j = 9 * i;
		MDataHandle outHandle = builder.addElement(i);
		MVector vectorA(prediction[0 + j], prediction[1 + j], prediction[2 + j]);
		MVector vectorB(prediction[3 + j], prediction[4 + j], prediction[5 + j]);
		MVector vectorC(prediction[6 + j], prediction[7 + j], prediction[8 + j]);
		outMatrix = getMatrixFromTriangle(vectorA, vectorB, vectorC, prevAim[i]);
		//outMatrix[3][0] = prediction[6 + j]; outMatrix[3][1] = prediction[7 + j]; outMatrix[3][2] = prediction[8 + j];
		outHandle.set(outMatrix);
	}
	
	stat = outputs_h.set(builder);
	outputs_h.setAllClean();
	data.setClean(plug);
	return MS::kSuccess;
}

void* mlRivet::creator()
{
	return new mlRivet();
}

MStatus mlRivet::initialize()
{
	MFnNumericAttribute nAttr;
	MFnTypedAttribute tAttr;
	MFnMatrixAttribute  mAttr;
	MFnEnumAttribute        enumAttr;
	MStatus	stat;
	deviceType = enumAttr.create("deviceType", "dt", 0, &stat);
	stat = enumAttr.addField("GPU", 0);
	stat = enumAttr.addField("CPU", 1);
	enumAttr.setHidden(false);
	enumAttr.setKeyable(true);
	stat = addAttribute(deviceType);

	MFnStringData fileFnStringData;
	MObject fileNameDefaultObject = fileFnStringData.create("");
	modelFilePath = tAttr.create("modelFile", "mf", MFnData::kString, fileNameDefaultObject);
	stat = tAttr.setStorable(true);
	stat = tAttr.setUsedAsFilename(true);
	stat = addAttribute(modelFilePath);

	inDataFilePath = tAttr.create("dataFile", "idf", MFnData::kString, fileNameDefaultObject);
	stat = tAttr.setStorable(true);
	stat = tAttr.setUsedAsFilename(true);
	stat = addAttribute(inDataFilePath);

	inputs = mAttr.create("inputs", "ins");
	mAttr.setArray(true);
	mAttr.setConnectable(true);
	stat = addAttribute(inputs);

	outputs = mAttr.create("outputs", "outputs");
	mAttr.setWritable(false);
	mAttr.setArray(true);
	mAttr.setUsesArrayDataBuilder(true);
	mAttr.setConnectable(true);
	stat = addAttribute(outputs);
	
	stat = attributeAffects(deviceType, outputs);
	stat = attributeAffects(inDataFilePath, outputs);
	stat = attributeAffects(inputs, outputs);

	return MS::kSuccess;
}


CNTK::FunctionPtr mlRivet::getModel(const std::wstring newFilePath)
{
	if (newFilePath == modelPath)
	{
		return modelPtr;
	}
	else
	{
		modelPath = newFilePath;
		const CNTK::DeviceDescriptor device = CNTK::DeviceDescriptor::CPUDevice();
		modelPtr = CNTK::Function::Load(modelPath, device, CNTK::ModelFormat::ONNX);
		return modelPtr;
	}
}

std::map<std::string, std::valarray<float>> mlRivet::getDataMap(const std::wstring inDataPath)
{
	if (dataPath == inDataPath)
	{
		return dataMap;
	}
	else
	{
		dataPath = inDataPath;
		std::string sInDataPath(dataPath.begin(), dataPath.end());
		zip *archive1 = get_archive(sInDataPath, ZIP_CREATE);
		dumpCsv(archive1, dataMap);
		zip_close(archive1);
		return dataMap;
	}
}