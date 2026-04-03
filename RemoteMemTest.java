import java.util.ArrayList;

/**
 * Test for disaggregated memory infrastructure.
 * Creates objects that survive multiple GC cycles (promoting to Old gen),
 * then triggers Young GC to exercise the G1SimulateRemoteEviction path.
 */
public class RemoteMemTest {
    static ArrayList<Object> longLived = new ArrayList<>();

    public static void main(String[] args) throws Exception {
        System.out.println("=== Remote Memory Infrastructure Test ===");

        // Phase 1: Create long-lived objects that will be promoted to Old
        System.out.println("Creating long-lived objects...");
        for (int i = 0; i < 10000; i++) {
            longLived.add(new byte[128]);
        }

        // Phase 2: Trigger several GCs to promote objects to Old
        System.out.println("Promoting objects to Old gen via GC cycles...");
        for (int i = 0; i < 5; i++) {
            // Allocate and discard (triggers Young GC)
            for (int j = 0; j < 50000; j++) {
                byte[] tmp = new byte[64];  // short-lived, will be collected
            }
        }

        // Phase 3: Trigger explicit GC to clean up
        System.gc();
        Thread.sleep(100);

        // Phase 4: More allocation to trigger Young GCs where eviction happens
        System.out.println("Triggering Young GCs (eviction should occur if G1SimulateRemoteEviction is set)...");
        for (int i = 0; i < 10; i++) {
            for (int j = 0; j < 100000; j++) {
                byte[] tmp2 = new byte[64];
            }
        }

        // Phase 5: Access long-lived objects (would trigger fetch if evicted)
        System.out.println("Accessing long-lived objects (fetch should occur for evicted objects)...");
        int sum = 0;
        for (Object obj : longLived) {
            byte[] arr = (byte[]) obj;
            sum += arr.length;
        }

        System.out.println("Sum of lengths: " + sum);
        System.out.println("=== Test Complete ===");
    }
}
