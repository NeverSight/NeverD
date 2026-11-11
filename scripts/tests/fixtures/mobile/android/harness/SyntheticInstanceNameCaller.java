import fixture.*;
public final class SyntheticInstanceNameCaller {
    public static Runnable create(Core owner, Runnable action) { return owner.task(action); }
}
