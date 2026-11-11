package fixture;
public final class Result<T> {
    public static int created;
    public T[] data;
    public int length;
    public Result(T[] input) { created++; data = input; length = input.length; }
}
