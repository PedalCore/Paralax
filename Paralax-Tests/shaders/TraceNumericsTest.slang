import Common.TraceNumerics;

struct Input
{
	float4 RowX;
	float4 RowY;
	float4 RowZ;
	float4 Lower;
	float4 Upper;
	float4 AnchorMaximum;
	float4 Offset;
	float4 Direction;
};

StructuredBuffer<Input> g_Input : register(t0);
RWStructuredBuffer<float4> g_Output : register(u0);

[shader("compute")]
[numthreads(1, 1, 1)]
void Arithmetic(uint3 tid : SV_DispatchThreadID)
{
	RequireTraceRTE32();
	Input input = g_Input[tid.x];
	TraceInterval a = { input.Lower.x, input.Upper.x }, b = { input.Lower.y, input.Upper.y };
	TraceInterval sum = TraceAdd(a, b), difference = TraceSubtract(a, b), product = TraceMultiply(a, b);
	g_Output[tid.x * 3] = float4(sum.Lo, sum.Hi, difference.Lo, difference.Hi);
	g_Output[tid.x * 3 + 1] = float4(product.Lo, product.Hi, TraceWrap(input.Lower.z).Lo, TraceWrap(input.Lower.z).Hi);
	precise float mul = input.Lower.x * input.Lower.y;
	precise float add = input.Lower.x + input.Lower.y;
	precise float sub = input.Lower.x - input.Lower.y;
	g_Output[tid.x * 3 + 2] = float4(mul, add, sub, 0);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void Bounds(uint3 tid : SV_DispatchThreadID)
{
	RequireTraceRTE32();
	Input input = g_Input[tid.x];
	TraceBox local = { { input.Lower.x, input.Upper.x }, { input.Lower.y, input.Upper.y }, { input.Lower.z, input.Upper.z } };
	TraceBox world = TraceTransformBox(local, input.RowX, input.RowY, input.RowZ);
	uint domain = TraceClassifyQuery(input.AnchorMaximum.xyz, input.Offset.xyz, input.Direction.xyz, input.AnchorMaximum.w);
	bool overlap = domain == 1 || (domain == 0 && TraceSegmentBox(world, input.AnchorMaximum.xyz, input.Offset.xyz, input.Direction.xyz, input.AnchorMaximum.w));
	g_Output[tid.x * 3] = float4(world.X.Lo, world.X.Hi, world.Y.Lo, world.Y.Hi);
	g_Output[tid.x * 3 + 1] = float4(world.Z.Lo, world.Z.Hi, float(domain), overlap ? 1.0 : 0.0);
	g_Output[tid.x * 3 + 2] = float4(TraceNominalRow(input.Lower.xyz, input.RowX), TraceNominalRow(input.Lower.xyz, input.RowY), TraceNominalRow(input.Lower.xyz, input.RowZ), 0);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void Gate(uint3 tid : SV_DispatchThreadID)
{
	RequireTraceRTE32();
	Input input = g_Input[tid.x];
	TraceBox box = { { input.Lower.x, input.Upper.x }, { input.Lower.y, input.Upper.y }, { input.Lower.z, input.Upper.z } };
	uint domain = TraceClassifyQuery(input.AnchorMaximum.xyz, input.Offset.xyz, input.Direction.xyz, input.AnchorMaximum.w);
	bool overlap = domain == 1 || (domain == 0 && TraceSegmentBox(box, input.AnchorMaximum.xyz, input.Offset.xyz, input.Direction.xyz, input.AnchorMaximum.w));
	g_Output[tid.x * 3] = float4(float(domain), overlap ? 1.0 : 0.0, 0, 0);
	g_Output[tid.x * 3 + 1] = 0;
	g_Output[tid.x * 3 + 2] = 0;
}

[shader("compute")]
[numthreads(1, 1, 1)]
void UnionBounds(uint3 tid : SV_DispatchThreadID)
{
	RequireTraceRTE32();
	Input input = g_Input[tid.x];
	TraceBox a = { { input.Lower.x, input.Upper.x }, { input.Lower.y, input.Upper.y }, { input.Lower.z, input.Upper.z } };
	TraceBox b = { { input.RowX.x, input.RowY.x }, { input.RowX.y, input.RowY.y }, { input.RowX.z, input.RowY.z } };
	TraceBox box = TraceUnion(a, b);
	g_Output[tid.x * 3] = float4(box.X.Lo, box.X.Hi, box.Y.Lo, box.Y.Hi);
	g_Output[tid.x * 3 + 1] = float4(box.Z.Lo, box.Z.Hi, 0, 0);
	g_Output[tid.x * 3 + 2] = 0;
}
