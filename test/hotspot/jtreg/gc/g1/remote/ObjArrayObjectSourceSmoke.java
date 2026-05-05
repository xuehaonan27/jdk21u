/*
 * @test
 * @summary Stress object-array references to ordinary objects under remote eviction.
 * @requires vm.gc.G1
 * @run main/othervm -Xms512m -Xmx512m -XX:+UseG1GC -XX:+UnlockDiagnosticVMOptions -XX:+G1SimulateRemoteEviction -XX:G1RemoteEvictionThreshold=0 -XX:+G1TagRefSites -XX:+G1RemoteAllowDenseObjectEviction -XX:+G1RemoteUseFastPhaseC -XX:+G1RemoteUseFastPhaseCSourceHints -XX:+G1RemoteTagObjArrayObjectSources -XX:+G1RemoteRepairFastPhaseCMisses -XX:G1RemoteFastPhaseCRepairMissLimit=200000 -XX:-G1RemoteUseRootCatchRelocation -XX:-UseCompressedOops -XX:-UseCompressedClassPointers -Xshare:off ObjArrayObjectSourceSmoke
 */

public class ObjArrayObjectSourceSmoke {
    static final int SHARDS = 96;
    static final int PER_SHARD = 8192;
    static final Object[][] ROOTS = new Object[SHARDS][];

    static final class Box {
        final int value;
        Box next;
        long pad0;
        long pad1;

        Box(int value) {
            this.value = value;
        }
    }

    public static void main(String[] args) {
        long expected = 0;
        Box prev = null;
        for (int s = 0; s < SHARDS; s++) {
            Object[] shard = new Object[PER_SHARD];
            ROOTS[s] = shard;
            for (int i = 0; i < PER_SHARD; i++) {
                int value = s * PER_SHARD + i;
                Box box = new Box(value);
                box.next = prev;
                prev = box;
                shard[i] = box;
                expected += value;
            }
        }

        for (int round = 0; round < 18; round++) {
            churn(round);
            System.gc();
            long actual = sum();
            if (actual != expected) {
                throw new AssertionError("sum mismatch round=" + round +
                        " expected=" + expected + " actual=" + actual);
            }
        }

        System.out.println("OK boxes=" + (SHARDS * PER_SHARD) + " sum=" + expected);
    }

    static long sum() {
        long total = 0;
        for (Object[] shard : ROOTS) {
            for (Object obj : shard) {
                Box box = (Box) obj;
                total += box.value;
                if (box.next != null) {
                    total += box.next.value & 1;
                    total -= box.next.value & 1;
                }
            }
        }
        return total;
    }

    static void churn(int round) {
        Object[] garbage = new Object[24_000];
        for (int i = 0; i < garbage.length; i++) {
            garbage[i] = new byte[2048 + ((i + round) & 127)];
        }
    }
}
