package fixture;
import java.util.List;
public final class Core extends Parent implements Pair<Left,Right> {
    public final List<? extends Left> first;
    public final List<? extends Right> second;
    public final long wide;
    public final float scale;
    public Core(List<? extends Left> first,List<? extends Right> second,long wide,float scale){
        super();
        Trace.step(6);
        this.first=first;this.second=second;this.wide=wide;this.scale=scale;
        Trace.firstSeen=first;Trace.secondSeen=second;Trace.wideSeen=wide;Trace.scaleSeen=scale;
        Trace.step(7);
        Trace.corePartial=this;
    }
    @SuppressWarnings("unchecked")
    public Core(List first,List second,long wide,float scale,int mask,Marker marker){
        if((mask&1)!=0){first=Trace.first();}
        if((mask&2)!=0){second=Trace.second();}
        if((mask&4)!=0){wide=Trace.wide();}
        if((mask&8)!=0){scale=Trace.scale();}
        this((List<? extends Left>)first,(List<? extends Right>)second,wide,scale);
    }
    public Core(){this(null,null,0L,0f,15,null);}
}
