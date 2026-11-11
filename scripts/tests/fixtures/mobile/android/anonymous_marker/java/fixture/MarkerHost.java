package fixture;

public class MarkerHost {
    static class Box {
        int value;
        private Box(int input) { value = input; }
    }

    public static int read(int input) { return new Box(input).value; }
}
