package fixture;
public class SdkRoot {
    public int scalar;
    public int sdkCalls;
    public RuntimeException failure;
    public SdkRoot() { scalar = 7; }
    public int getState() {
        sdkCalls++;
        if (failure != null) throw failure;
        return scalar;
    }
}
