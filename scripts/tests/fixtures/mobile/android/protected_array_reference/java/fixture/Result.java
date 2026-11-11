package fixture;
public final class Result {
    public static int created;
    public boolean[] data;
    public int length;
    public Result(boolean[] input) { created++; data = input; length = input.length; }
}
