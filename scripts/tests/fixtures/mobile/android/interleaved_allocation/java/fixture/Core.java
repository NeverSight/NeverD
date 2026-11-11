package fixture;
public final class Core {
    private Core() {}
    public static Target make(Provider provider,long bits) {
        switch(provider.branch) {
        case 0:return null;
        case 1:return new Target(provider.alternate,provider.finish(bits));
        default:return new Target(provider.flag==0?provider.first:provider.alternate,provider.finish(bits));
        }
    }
}
