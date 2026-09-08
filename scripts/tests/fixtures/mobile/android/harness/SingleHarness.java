import fixture.Peer;

public class SingleHarness {
    public static void main(String[] arguments) {
        int[] values = {Integer.MIN_VALUE, -32769, -1, 0, 1, 32768, Integer.MAX_VALUE};
        for (int i = 0; i < values.length; i++) System.out.println("twice:" + i + "=" + Peer.twice(values[i]));
        System.out.println("greeting=" + (Peer.greeting().equals("neverd") ? 1 : 0));
    }
}
