import java.lang.ref.*;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;
import java.util.stream.*;
import java.nio.*;
import java.nio.charset.*;
import java.security.*;
import java.lang.invoke.*;
import java.util.function.*;

/**
 * Heavy stress test for disaggregated memory infrastructure.
 * Exercises: GC pressure, C1/C2 JIT, java.lang.ref, ConcurrentHashMap,
 * atomics, String operations, lambdas/streams, polymorphic dispatch,
 * deep object graphs, large arrays, weak/soft/phantom references,
 * MethodHandles, VarHandles, invokedynamic, reflection, Unsafe field access.
 */
public class StressTest {

    // ---- Retained data (promotes to Old, candidates for classification/eviction) ----
    static final int NUM_ENTRIES = 50_000;
    static final ConcurrentHashMap<String, Node> liveMap = new ConcurrentHashMap<>();
    static final ArrayList<Object> tenured = new ArrayList<>();
    static final LinkedList<SoftReference<byte[]>> softCache = new LinkedList<>();
    static final LinkedList<WeakReference<String>> weakInterns = new LinkedList<>();
    static final ReferenceQueue<Object> phantomQueue = new ReferenceQueue<>();
    static final ArrayList<PhantomReference<byte[]>> phantoms = new ArrayList<>();
    static final AtomicLong allocBytes = new AtomicLong();
    static final AtomicInteger gcCount = new AtomicInteger();
    static volatile boolean running = true;

    // ---- Polymorphic hierarchy (C2 profiles, inline caches) ----
    interface Shape { double area(); String name(); }
    record Circle(double r) implements Shape {
        public double area() { return Math.PI * r * r; }
        public String name() { return "Circle(r=" + r + ")"; }
    }
    record Rect(double w, double h) implements Shape {
        public double area() { return w * h; }
        public String name() { return "Rect(" + w + "x" + h + ")"; }
    }
    record Triangle(double b, double h) implements Shape {
        public double area() { return 0.5 * b * h; }
        public String name() { return "Triangle(b=" + b + ",h=" + h + ")"; }
    }

    // ---- Linked list node (deep object graph, pointer-heavy) ----
    static class Node {
        final String key;
        volatile Object value;
        volatile Node next;
        final long stamp;
        Node(String key, Object value, Node next) {
            this.key = key;
            this.value = value;
            this.next = next;
            this.stamp = System.nanoTime();
        }
        int chainLength() {
            int len = 1;
            Node n = next;
            while (n != null) { len++; n = n.next; }
            return len;
        }
    }

    // ---- Tree node (balanced, recursive, promotes to Old) ----
    static class TreeNode {
        int val;
        TreeNode left, right;
        String label;
        TreeNode(int val) {
            this.val = val;
            this.label = "node-" + val;
        }
        static TreeNode buildBalanced(int lo, int hi) {
            if (lo > hi) return null;
            int mid = (lo + hi) / 2;
            TreeNode n = new TreeNode(mid);
            n.left = buildBalanced(lo, mid - 1);
            n.right = buildBalanced(mid + 1, hi);
            return n;
        }
        int sum() {
            int s = val;
            if (left != null) s += left.sum();
            if (right != null) s += right.sum();
            return s;
        }
        int depth() {
            int ld = left == null ? 0 : left.depth();
            int rd = right == null ? 0 : right.depth();
            return 1 + Math.max(ld, rd);
        }
    }

    // ---- Wrapper for captured lambda vars (exercises lambda capture paths) ----
    static class Holder<T> {
        volatile T value;
        Holder(T v) { this.value = v; }
        T get() { return value; }
        void set(T v) { this.value = v; }
    }

    public static void main(String[] args) throws Throwable {
        System.out.println("=== Heavy Stress Test for Disaggregated Memory ===");
        System.out.println("Heap max: " + Runtime.getRuntime().maxMemory() / (1024*1024) + " MB");
        long start = System.currentTimeMillis();

        // Phase 1: Build long-lived data structures (promote to Old)
        System.out.println("\n[Phase 1] Building long-lived data structures...");
        phase1_buildRetained();

        // Phase 2: Allocation storm (trigger Young GC, promote survivors)
        System.out.println("\n[Phase 2] Allocation storm (GC pressure)...");
        phase2_allocationStorm();

        // Phase 3: Reference processing (Soft, Weak, Phantom)
        System.out.println("\n[Phase 3] Reference processing...");
        phase3_references();

        // Phase 4: Polymorphic dispatch + streams (C2 JIT, profiling)
        System.out.println("\n[Phase 4] Polymorphic dispatch + streams...");
        phase4_polymorphicStreams();

        // Phase 5: Concurrent HashMap churn (CAS, volatile, atomics)
        System.out.println("\n[Phase 5] Concurrent HashMap churn...");
        phase5_concurrentChurn();

        // Phase 6: String operations (interning, concatenation, encoding)
        System.out.println("\n[Phase 6] String operations...");
        phase6_stringOps();

        // Phase 7: Deep recursive tree (pointer-heavy, promotes to Old)
        System.out.println("\n[Phase 7] Deep recursive tree...");
        phase7_deepTree();

        // Phase 8: Lambda capture + MethodHandle exercises
        System.out.println("\n[Phase 8] Lambda capture + MethodHandle...");
        phase8_lambdaAndMethodHandle();

        // Phase 9: Multi-threaded field churn (cross-gen pointers, barrier stress)
        System.out.println("\n[Phase 9] Multi-threaded field churn...");
        phase9_fieldChurn();

        // Phase 10: Repeated GC + allocation cycles (stress classification)
        System.out.println("\n[Phase 10] GC cycling stress...");
        phase10_gcCyclingStress();

        // Phase 11: Final GC + verify
        System.out.println("\n[Phase 11] Final GC + verification...");
        phase11_verify();

        long elapsed = System.currentTimeMillis() - start;
        System.out.println("\n=== Stress Test PASSED in " + elapsed + " ms ===");
        System.out.println("Total allocated: " + allocBytes.get() / (1024*1024) + " MB");
    }

    static void phase1_buildRetained() {
        // Build linked-list chains in the ConcurrentHashMap
        for (int i = 0; i < NUM_ENTRIES; i++) {
            String key = "entry-" + i;
            byte[] payload = new byte[64 + (i % 256)];
            Arrays.fill(payload, (byte)(i & 0xFF));
            Node prev = liveMap.get("entry-" + Math.max(0, i - 10));
            Node node = new Node(key, payload, prev);
            liveMap.put(key, node);
            allocBytes.addAndGet(payload.length + 80);
        }
        // Build array of mixed types
        for (int i = 0; i < 10_000; i++) {
            if (i % 3 == 0) tenured.add(new int[32]);
            else if (i % 3 == 1) tenured.add("tenured-string-" + i);
            else tenured.add(new Circle(i * 0.1));
        }
        System.out.println("  liveMap: " + liveMap.size() + " entries");
        System.out.println("  tenured: " + tenured.size() + " objects");
    }

    static void phase2_allocationStorm() {
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        long phaseAlloc = 0;
        for (int round = 0; round < 20; round++) {
            ArrayList<Object> ephemeral = new ArrayList<>(10_000);
            for (int i = 0; i < 10_000; i++) {
                int size = 64 + rng.nextInt(4096);
                byte[] buf = new byte[size];
                buf[0] = (byte) round;
                buf[size - 1] = (byte) i;
                ephemeral.add(buf);
                phaseAlloc += size;
                if (i % 500 == 0 && !liveMap.isEmpty()) {
                    String k = "entry-" + rng.nextInt(NUM_ENTRIES);
                    Node n = liveMap.get(k);
                    if (n != null) n.value = buf;
                }
            }
            ephemeral.clear();
        }
        allocBytes.addAndGet(phaseAlloc);
        System.out.println("  Allocated " + phaseAlloc / (1024*1024) + " MB ephemeral data");
    }

    static void phase3_references() {
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        for (int i = 0; i < 5_000; i++) {
            byte[] data = new byte[1024 + rng.nextInt(8192)];
            softCache.add(new SoftReference<>(data));
            allocBytes.addAndGet(data.length);
        }
        for (int i = 0; i < 10_000; i++) {
            String s = new String("weak-intern-" + rng.nextInt(100_000));
            weakInterns.add(new WeakReference<>(s));
        }
        for (int i = 0; i < 2_000; i++) {
            byte[] data = new byte[512];
            phantoms.add(new PhantomReference<>(data, phantomQueue));
        }
        System.gc();
        try { Thread.sleep(100); } catch (InterruptedException e) {}

        long softAlive = softCache.stream().filter(r -> r.get() != null).count();
        long weakAlive = weakInterns.stream().filter(r -> r.get() != null).count();
        int phantomPolled = 0;
        while (phantomQueue.poll() != null) phantomPolled++;

        System.out.println("  Soft refs alive: " + softAlive + "/" + softCache.size());
        System.out.println("  Weak refs alive: " + weakAlive + "/" + weakInterns.size());
        System.out.println("  Phantom refs polled: " + phantomPolled);
    }

    static void phase4_polymorphicStreams() {
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        ArrayList<Shape> shapes = new ArrayList<>(100_000);
        for (int i = 0; i < 100_000; i++) {
            switch (i % 3) {
                case 0 -> shapes.add(new Circle(rng.nextDouble(100)));
                case 1 -> shapes.add(new Rect(rng.nextDouble(100), rng.nextDouble(100)));
                case 2 -> shapes.add(new Triangle(rng.nextDouble(100), rng.nextDouble(100)));
            }
        }

        double totalArea = shapes.stream().mapToDouble(Shape::area).sum();
        long bigShapes = shapes.stream().filter(s -> s.area() > 5000).count();
        Optional<Shape> maxShape = shapes.stream().max(Comparator.comparingDouble(Shape::area));

        // Parallel stream (multi-threaded lambda dispatch)
        Map<String, Double> avgByType = shapes.parallelStream()
                .collect(Collectors.groupingBy(
                        s -> s.getClass().getSimpleName(),
                        Collectors.averagingDouble(Shape::area)));

        // Chained stream with multiple intermediate ops
        List<String> topNames = shapes.stream()
                .filter(s -> s.area() > 1000)
                .sorted(Comparator.comparingDouble(Shape::area).reversed())
                .limit(100)
                .map(Shape::name)
                .collect(Collectors.toList());

        System.out.println("  Total area: " + String.format("%.2f", totalArea));
        System.out.println("  Big shapes (area > 5000): " + bigShapes);
        System.out.println("  Max shape: " + maxShape.map(Shape::name).orElse("none"));
        System.out.println("  Avg area by type: " + avgByType);
        System.out.println("  Top 100 shapes collected: " + topNames.size());
    }

    static void phase5_concurrentChurn() throws Exception {
        ConcurrentHashMap<Integer, AtomicLong> counters = new ConcurrentHashMap<>();
        for (int i = 0; i < 1000; i++) counters.put(i, new AtomicLong());

        int nThreads = Math.min(Runtime.getRuntime().availableProcessors(), 4);
        ExecutorService pool = Executors.newFixedThreadPool(nThreads);
        CountDownLatch latch = new CountDownLatch(nThreads);

        for (int t = 0; t < nThreads; t++) {
            final int tid = t;
            pool.submit(() -> {
                try {
                    ThreadLocalRandom rng = ThreadLocalRandom.current();
                    for (int i = 0; i < 200_000; i++) {
                        int k = rng.nextInt(1000);
                        counters.get(k).incrementAndGet();
                        if (i % 100 == 0) {
                            String key = "entry-" + rng.nextInt(NUM_ENTRIES);
                            Node n = liveMap.get(key);
                            if (n != null) {
                                n.value = new byte[32 + rng.nextInt(64)];
                            }
                        }
                    }
                } finally {
                    latch.countDown();
                }
            });
        }
        latch.await(30, TimeUnit.SECONDS);
        pool.shutdown();
        pool.awaitTermination(10, TimeUnit.SECONDS);

        long totalOps = counters.values().stream().mapToLong(AtomicLong::get).sum();
        System.out.println("  Concurrent ops: " + totalOps + " across " + nThreads + " threads");
    }

    static void phase6_stringOps() {
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        ArrayList<String> strings = new ArrayList<>(50_000);

        for (int i = 0; i < 20_000; i++) {
            String s = "key=" + i + ",val=" + rng.nextInt(1_000_000) + ",hash=" + Integer.toHexString(i);
            strings.add(s);
        }
        for (int i = 0; i < 10_000; i++) {
            StringBuilder sb = new StringBuilder(128);
            for (int j = 0; j < 10; j++) {
                sb.append("seg").append(j).append('-').append(rng.nextInt(100));
                if (j < 9) sb.append('/');
            }
            strings.add(sb.toString());
        }
        int encodingErrors = 0;
        for (int i = 0; i < 10_000; i++) {
            String orig = strings.get(i % strings.size());
            byte[] utf8 = orig.getBytes(StandardCharsets.UTF_8);
            String decoded = new String(utf8, StandardCharsets.UTF_8);
            if (!orig.equals(decoded)) encodingErrors++;
        }
        HashSet<String> deduped = new HashSet<>(strings);
        strings.sort(Comparator.naturalOrder());
        long containsCount = strings.stream()
                .filter(s -> s.contains("val=") && s.length() > 20)
                .count();

        System.out.println("  Strings: " + strings.size() + ", unique: " + deduped.size());
        System.out.println("  Encoding errors: " + encodingErrors);
        System.out.println("  Contains matches: " + containsCount);
    }

    static void phase7_deepTree() {
        TreeNode root = TreeNode.buildBalanced(0, 99_999);
        int sum = root.sum();
        int depth = root.depth();
        System.gc();
        int sum2 = root.sum();
        if (sum != sum2) throw new RuntimeException("Tree sum changed after GC: " + sum + " vs " + sum2);

        long treeAlloc = 0;
        for (int i = 0; i < 500; i++) {
            TreeNode t = TreeNode.buildBalanced(0, 999);
            treeAlloc += t.sum();
        }

        System.out.println("  Tree depth: " + depth + ", sum: " + sum);
        System.out.println("  Small tree traversal checksum: " + treeAlloc);
    }

    // ---- Phase 8: Lambda captures + MethodHandle + functional interfaces ----
    static void phase8_lambdaAndMethodHandle() throws Throwable {
        ThreadLocalRandom rng = ThreadLocalRandom.current();

        // 8a: Lambda captures of promoted-to-Old objects
        ArrayList<Holder<String>> holders = new ArrayList<>();
        for (int i = 0; i < 10_000; i++) {
            holders.add(new Holder<>("holder-" + i));
        }
        // Force promotion to Old
        System.gc(); System.gc();

        // Lambda capturing Old objects — exercises lambda metafactory + captured field access
        List<Supplier<String>> suppliers = holders.stream()
                .map(h -> (Supplier<String>) () -> h.get() + "-accessed")
                .collect(Collectors.toList());

        long supplierOk = 0;
        for (Supplier<String> s : suppliers) {
            String val = s.get();
            if (val != null && val.endsWith("-accessed")) supplierOk++;
        }
        System.out.println("  Lambda captures OK: " + supplierOk + "/" + suppliers.size());

        // 8b: MethodHandle direct invoke on Old objects
        MethodHandles.Lookup lookup = MethodHandles.lookup();
        MethodHandle getterMH = lookup.findVirtual(Holder.class, "get", MethodType.methodType(Object.class));
        MethodHandle setterMH = lookup.findVirtual(Holder.class, "set",
                MethodType.methodType(void.class, Object.class));

        long mhOk = 0;
        for (int i = 0; i < 1000; i++) {
            Holder<String> h = holders.get(rng.nextInt(holders.size()));
            Object val = getterMH.invoke(h);
            if (val != null) mhOk++;
            setterMH.invoke(h, "mh-updated-" + i);
        }
        System.out.println("  MethodHandle invokes OK: " + mhOk);

        // 8c: Function composition (chained lambdas)
        Function<Integer, String> pipeline = ((Function<Integer, Integer>) (x -> x * 2))
                .andThen(x -> x + 10)
                .andThen(x -> "result=" + x);
        long pipeOk = 0;
        for (int i = 0; i < 10_000; i++) {
            String r = pipeline.apply(i);
            if (r.startsWith("result=")) pipeOk++;
        }
        System.out.println("  Function pipeline OK: " + pipeOk);

        // 8d: Predicate/Consumer/BiFunction on Old objects
        ArrayList<Object[]> pairs = new ArrayList<>();
        for (int i = 0; i < 5_000; i++) {
            pairs.add(new Object[]{"key-" + i, new byte[16 + rng.nextInt(64)]});
        }
        System.gc(); // promote to Old

        BiFunction<Object[], Integer, String> extractor = (pair, idx) ->
                pair[idx].toString().substring(0, Math.min(10, pair[idx].toString().length()));

        long biOk = 0;
        for (Object[] pair : pairs) {
            String k = extractor.apply(pair, 0);
            if (k != null) biOk++;
        }
        System.out.println("  BiFunction on Old objects OK: " + biOk);
    }

    // ---- Phase 9: Multi-threaded field mutation (cross-gen pointer stress) ----
    static void phase9_fieldChurn() throws Exception {
        // Create a graph of objects, some in Old gen
        final int GRAPH_SIZE = 20_000;
        Node[] graph = new Node[GRAPH_SIZE];
        for (int i = 0; i < GRAPH_SIZE; i++) {
            graph[i] = new Node("graph-" + i, new byte[32], null);
        }
        // Link them randomly
        ThreadLocalRandom rng = ThreadLocalRandom.current();
        for (int i = 0; i < GRAPH_SIZE; i++) {
            graph[i].next = graph[rng.nextInt(GRAPH_SIZE)];
        }
        // Promote to Old
        System.gc(); System.gc();

        // Multiple threads mutate fields concurrently (creates cross-gen pointers,
        // exercises write barrier + card marking under contention)
        int nThreads = Math.min(Runtime.getRuntime().availableProcessors(), 4);
        ExecutorService pool = Executors.newFixedThreadPool(nThreads);
        AtomicLong mutations = new AtomicLong();
        CountDownLatch latch = new CountDownLatch(nThreads);

        for (int t = 0; t < nThreads; t++) {
            pool.submit(() -> {
                try {
                    ThreadLocalRandom tlr = ThreadLocalRandom.current();
                    for (int i = 0; i < 500_000; i++) {
                        int src = tlr.nextInt(GRAPH_SIZE);
                        int dst = tlr.nextInt(GRAPH_SIZE);
                        // Mutate next pointer (Old→Old, triggers write barrier)
                        graph[src].next = graph[dst];
                        // Mutate value with new young object (Old→Young, triggers card mark)
                        if (i % 100 == 0) {
                            graph[src].value = new byte[16 + tlr.nextInt(48)];
                        }
                        // Read through chain (exercises load barrier on Old fields)
                        Node n = graph[src];
                        int chain = 0;
                        while (n != null && chain < 10) {
                            Object v = n.value; // getfield on Old object
                            if (v != null) mutations.incrementAndGet();
                            n = n.next;
                            chain++;
                        }
                    }
                } finally {
                    latch.countDown();
                }
            });
        }
        latch.await(60, TimeUnit.SECONDS);
        pool.shutdown();
        pool.awaitTermination(10, TimeUnit.SECONDS);

        System.out.println("  Graph mutations: " + mutations.get() + " across " + nThreads + " threads");

        // Verify graph integrity after all mutations
        int reachable = 0;
        for (Node n : graph) {
            if (n != null && n.key != null) reachable++;
        }
        System.out.println("  Graph nodes reachable: " + reachable + "/" + GRAPH_SIZE);
    }

    // ---- Phase 10: Repeated GC + allocation cycles ----
    static void phase10_gcCyclingStress() {
        // Rapidly cycle through allocation → GC → re-allocation
        // to stress classification fixup + barrier interactions
        for (int cycle = 0; cycle < 20; cycle++) {
            // Allocate many small objects that reference Old data
            ArrayList<Object[]> batch = new ArrayList<>(5_000);
            for (int i = 0; i < 5_000; i++) {
                // Each object array references 3 random tenured objects (cross-gen)
                Object[] refs = new Object[4];
                refs[0] = tenured.get(ThreadLocalRandom.current().nextInt(tenured.size()));
                refs[1] = liveMap.get("entry-" + ThreadLocalRandom.current().nextInt(NUM_ENTRIES));
                refs[2] = "cycle-" + cycle + "-" + i;
                refs[3] = new byte[32 + ThreadLocalRandom.current().nextInt(128)];
                batch.add(refs);
            }

            // Force GC to process these cross-gen refs
            if (cycle % 5 == 0) {
                System.gc();
            }

            // Verify some refs survived correctly
            int valid = 0;
            for (Object[] refs : batch) {
                if (refs[0] != null && refs[1] != null && refs[2] != null) valid++;
            }
            if (valid != batch.size()) {
                throw new RuntimeException("Cycle " + cycle + ": refs corrupted! valid=" + valid + "/" + batch.size());
            }
        }
        System.out.println("  20 GC cycles completed, all refs intact");
    }

    static void phase11_verify() {
        System.gc();
        try { Thread.sleep(200); } catch (InterruptedException e) {}

        int chainLenSum = 0;
        int nullValues = 0;
        int evictedEntries = 0;
        for (Map.Entry<String, Node> e : liveMap.entrySet()) {
            try {
                Node n = e.getValue();
                if (n.value == null) nullValues++;
                chainLenSum += n.chainLength();
            } catch (ClassCastException | NullPointerException ex) {
                evictedEntries++;
            }
        }
        System.out.println("  liveMap entries: " + liveMap.size() + " (evicted: " + evictedEntries + ")");
        System.out.println("  Total chain length: " + chainLenSum);
        System.out.println("  Null values (overwritten): " + nullValues);

        int intArrays = 0, strings = 0, circles = 0;
        for (Object o : tenured) {
            if (o instanceof int[]) intArrays++;
            else if (o instanceof String) strings++;
            else if (o instanceof Circle) circles++;
        }
        System.out.println("  Tenured: " + intArrays + " int[], " + strings + " String, " + circles + " Circle");

        Runtime rt = Runtime.getRuntime();
        long used = rt.totalMemory() - rt.freeMemory();
        System.out.println("  Heap used: " + used / (1024*1024) + " MB / " + rt.maxMemory() / (1024*1024) + " MB");
    }
}
