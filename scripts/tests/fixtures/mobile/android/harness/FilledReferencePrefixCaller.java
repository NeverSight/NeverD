package fixture;
public final class FilledReferencePrefixCaller {
    public static Core create(int operation,Object first,Object second) {
        switch(operation) {
        case 0: return new Core(first,second);
        case 1: return new Core((Object[])first,(Object[])second);
        case 2: return new Core(first,second,1);
        default: return new Core((Object[])first,(Object[])second,1);
        }
    }
}
