/*
 * Copyright Amazon.com Inc. or its affiliates. All Rights Reserved.
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This code is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 only, as
 * published by the Free Software Foundation.
 *
 * This code is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * version 2 for more details (a copy is included in the LICENSE file that
 * accompanied this code).
 *
 * You should have received a copy of the GNU General Public License version
 * 2 along with this work; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Please contact Oracle, 500 Oracle Parkway, Redwood Shores, CA 94065 USA
 * or visit www.oracle.com if you need additional information or have any
 * questions.
 *
 */

package org.openjdk.bench.vm.compiler;

import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.Random;

import java.util.concurrent.TimeUnit;

import org.openjdk.jmh.annotations.Benchmark;
import org.openjdk.jmh.annotations.BenchmarkMode;
import org.openjdk.jmh.annotations.CompilerControl;
import org.openjdk.jmh.annotations.Fork;
import org.openjdk.jmh.annotations.Level;
import org.openjdk.jmh.annotations.Mode;
import org.openjdk.jmh.annotations.OutputTimeUnit;
import org.openjdk.jmh.annotations.Param;
import org.openjdk.jmh.annotations.Scope;
import org.openjdk.jmh.annotations.Setup;
import org.openjdk.jmh.annotations.State;
import org.openjdk.jmh.annotations.Warmup;

import org.openjdk.bench.util.InMemoryJavaCompiler;

import jdk.test.whitebox.WhiteBox;
import jdk.test.whitebox.code.BlobType;
import jdk.test.whitebox.code.NMethod;

@BenchmarkMode(Mode.AverageTime)
@OutputTimeUnit(TimeUnit.MICROSECONDS)
@State(Scope.Benchmark)
@Fork(value = 3)

// jvmArgs = {
//     "-XX:+UnlockDiagnosticVMOptions",
//     "-XX:+UnlockExperimentalVMOptions",
//     "-XX:+WhiteBoxAPI",
//     "-Xbootclasspath/a:lib-test/wb.jar",
//     "-XX:CompileCommand=dontinline,A::sum",
//     "-XX:-UseCodeCacheFlushing",
//     "-XX:-TieredCompilation",
//     "-XX:+SegmentedCodeCache",
//     "-XX:ReservedCodeCacheSize=512m",
//     "-XX:InitialCodeCacheSize=512m",
//     "-XX:+UseSerialGC",
//     "-XX:+PrintCodeCache"
// }

public class UnorderedCodeCache {

    private static final int C2_LEVEL = 4;

    static byte[] num1;
    static byte[] num2;

    @State(Scope.Thread)
    public static class ThreadState {
        byte[] result;

        @Setup
        public void setup() {
            result = new byte[num1.length + 1];
        }
    }

    private static Object WB;
    private static long compilerThreads;

    @Param({"5000", "20000"})
    public int totalCallsPerIteration;

    private ArrayList<TestMethod> methods = new ArrayList<>();
    private double[] methodWeights;
    private int[] callSequence;
    private Random weightRandom;

    private static byte[] genNum(Random random, int digitCount) {
        byte[] num = new byte[digitCount];
        int d;
        do {
            d = random.nextInt(10);
        } while (d == 0);

        num[0] = (byte)d;
        for (int i = 1; i < digitCount; ++i) {
            num[i] = (byte)random.nextInt(10);
        }
        return num;
    }

    private static void initWhiteBox() {
        WB = WhiteBox.getWhiteBox();
        compilerThreads = (Long) getWhiteBox().getVMFlag("CICompilerCount");
    }

    private static void initNums() {
        final long seed = 8374592837465123L;
        Random random = new Random(seed);

        final int digitCount = 40;
        num1 = genNum(random, digitCount);
        num2 = genNum(random, digitCount);
    }

    private static WhiteBox getWhiteBox() {
        return (WhiteBox)WB;
    }

    private void calculateWeights() {
        methodWeights = new double[methods.size()];
        double sum = 0.0;

        // Calculate raw Zipf weights: weight[i] = 1 / rank
        for (int i = 0; i < methods.size(); i++) {
            double rank = i + 1; // 1-based ranking
            methodWeights[i] = 1.0 / rank;
            sum += methodWeights[i];
        }

        // Normalize weights to sum to 1.0
        for (int i = 0; i < methods.size(); i++) {
            methodWeights[i] /= sum;
        }

        // Shuffle methodWeights in place to randomize assignment
        Random shuffleRandom = new Random(42);
        for (int i = methods.size() - 1; i > 0; i--) {
            int j = shuffleRandom.nextInt(i + 1);
            double temp = methodWeights[i];
            methodWeights[i] = methodWeights[j];
            methodWeights[j] = temp;
        }
    }

    private void generateCallSequence() {
        callSequence = new int[totalCallsPerIteration];
        weightRandom = new Random(42); // Fixed seed for reproducibility

        // Convert weights to cumulative probabilities
        double[] cumulativeWeights = new double[methods.size()];
        cumulativeWeights[0] = methodWeights[0];
        for (int i = 1; i < methods.size(); i++) {
            cumulativeWeights[i] = cumulativeWeights[i - 1] + methodWeights[i];
        }

        // Generate call sequence using weighted random selection
        for (int i = 0; i < totalCallsPerIteration; i++) {
            double random = weightRandom.nextDouble();

            // Binary search to find the method index
            int methodIndex = 0;
            for (int j = 0; j < methods.size(); j++) {
                if (random <= cumulativeWeights[j]) {
                    methodIndex = j;
                    break;
                }
            }
            callSequence[i] = methodIndex;
        }
    }

    private static final class TestMethod {
        private static final String CLASS_NAME = "A";
        private static final String METHOD_TO_COMPILE = "sum";
        private static final String JAVA_CODE = """
        public class A {

            public static void sum(byte[] n1, byte[] n2, byte[] out) {
                final int digitCount = n1.length;
                int carry = 0;
                for (int i = digitCount - 1; i >= 0; --i) {
                    int sum = n1[i] + n2[i] + carry;
                    out[i] = (byte)(sum % 10);
                    carry = sum / 10;
                }
                if (carry != 0) {
                    for (int i = digitCount; i > 0; --i) {
                        out[i] = out[i - 1];
                    }
                    out[0] = (byte)carry;
                }
            }
        }""";

        private static final byte[] BYTE_CODE;

        static {
            BYTE_CODE = InMemoryJavaCompiler.compile(CLASS_NAME, JAVA_CODE);
        }

        private final Method method;

        private static ClassLoader createClassLoaderFor() {
            return new ClassLoader() {
                @Override
                public Class<?> loadClass(String name) throws ClassNotFoundException {
                    if (!name.equals(CLASS_NAME)) {
                        return super.loadClass(name);
                    }

                    return defineClass(name, BYTE_CODE, 0, BYTE_CODE.length);
                }
            };
        }

        public TestMethod() throws Exception {
            var cl = createClassLoaderFor().loadClass(CLASS_NAME);
            method = cl.getMethod(METHOD_TO_COMPILE, byte[].class, byte[].class, byte[].class);
            getWhiteBox().testSetDontInlineMethod(method, true);
        }

        public void profile(byte[] num1, byte[] num2, byte[] result) throws Exception {
            method.invoke(null, num1, num2, result);
            getWhiteBox().markMethodProfiled(method);
        }

        public void invoke(byte[] num1, byte[] num2, byte[] result) throws Exception {
            method.invoke(null, num1, num2, result);
        }

        public void enqueueForC2Compilation() throws Exception {
            getWhiteBox().enqueueMethodForCompilation(method, C2_LEVEL);
        }

        public boolean isC2Compiled() {
            return getWhiteBox().getMethodCompilationLevel(method) == C2_LEVEL;
        }

        public NMethod getNMethod() {
            return NMethod.get(method, false);
        }
    }

    private void generateCode() throws Exception {
        initNums();

        byte[] result = new byte[num1.length + 1];

        // Compile while more than 25% free
        while ((double)getWhiteBox().getHeapUnallocatedCapacity(BlobType.MethodNonProfiled.id) / getWhiteBox().getHeapMaxCapacity(BlobType.MethodNonProfiled.id) > 0.25) {
            TestMethod m = new TestMethod();
            m.profile(num1, num2, result);
            m.enqueueForC2Compilation();
            methods.add(m);

            while (getWhiteBox().getCompileQueueSize(C2_LEVEL) > compilerThreads * 2) {
                Thread.onSpinWait(); // Wait to queue methods until room in queue
            }
        }

        while (getWhiteBox().getCompileQueueSize(C2_LEVEL) > 0) {
            Thread.onSpinWait(); // Flush queue
        }

        // Remove methods that could have failed compilation
        methods.removeIf(m -> !m.isC2Compiled());

        // Sort methods based on address in CodeCache
        methods.sort((a, b) -> Long.compare(a.getNMethod().address, b.getNMethod().address));
    }

    private void compileCallMethods() throws Exception {
        var threadState = new ThreadState();
        threadState.setup();
        callMethods(threadState);
        Method method = UnorderedCodeCache.class.getDeclaredMethod("callMethods", ThreadState.class);
        getWhiteBox().markMethodProfiled(method);
        getWhiteBox().enqueueMethodForCompilation(method, C2_LEVEL);
        while (getWhiteBox().isMethodQueuedForCompilation(method)) {
            Thread.onSpinWait();
        }
        if (getWhiteBox().getMethodCompilationLevel(method) != C2_LEVEL) {
            throw new IllegalStateException("Method UnorderedCodeCache::callMethods is not compiled by C2.");
        }
        getWhiteBox().testSetDontInlineMethod(method, true);
    }

    @Setup(Level.Trial)
    public void setupCodeCache() throws Exception {
        initWhiteBox();
        generateCode();
        calculateWeights();
        generateCallSequence();
        compileCallMethods();
    }

    @CompilerControl(CompilerControl.Mode.DONT_INLINE)
    private void callMethods(ThreadState s) throws Exception {
        for (int i = 0; i < callSequence.length; i++) {
            int methodIndex = callSequence[i];
            methods.get(methodIndex).invoke(num1, num2, s.result);
        }
    }

    @Benchmark
    @Warmup(iterations = 20)
    public void runMethodsWithReflection(ThreadState s) throws Exception {
        callMethods(s);
    }
}
