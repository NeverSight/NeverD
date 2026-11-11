package fixture;
public final class Changer {
    public static String value;
    static {if(Trace.targetInitializations!=1)throw new AssertionError("owned reused allocation read");Trace.changerInitializations++;Provider.field=Trace.replacement;value=Trace.middle;}
}
