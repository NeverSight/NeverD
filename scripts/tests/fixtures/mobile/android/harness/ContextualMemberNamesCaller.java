import fixture.yield.*;
public final class ContextualMemberNamesCaller {
    public static int call(Core target, int value) { return Core.call(target, value); }
    public static int inherited(Base target, int value) { return target.yield(value); }
}
