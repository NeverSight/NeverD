package fixture;
public final class Core {
    public final boolean value;
    public final Core peer;
    public final Object token;
    public Core(boolean value,Core peer,Object token) {
        Trace.partial=this; Trace.hit(5);
        this.value=value; this.peer=peer; this.token=token; Trace.hit(7);
    }
    public Core(boolean explicit,int mask,Core fallback,Object token) {
        Core selected; boolean chosen;
        if((mask&1)!=0) { selected=Defaults.peer; chosen=selected.read(); }
        else { selected=fallback; chosen=explicit; }
        this(chosen,selected,token);
    }
    private boolean read() {
        Trace.seen=this; Trace.hit(2);
        boolean result=value; Trace.hit(3);
        Defaults.peer=Trace.replacement; Trace.hit(4);
        return result;
    }
}
