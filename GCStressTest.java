import java.util.ArrayList;

public class GCStressTest {
    public static void main(String[] args) {
        ArrayList<byte[]> list = new ArrayList<>();
        for (int i = 0; i < 500000; i++) {
            list.add(new byte[64]);
            if (i % 10000 == 0) {
                list.subList(0, list.size() / 2).clear();
                System.gc();
            }
        }
        System.out.println("GC stress test passed: " + list.size() + " objects");
    }
}
