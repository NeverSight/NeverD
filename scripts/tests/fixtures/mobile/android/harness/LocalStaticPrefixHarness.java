import fixture.*;
import kotlin.jvm.functions.Function1;
public class LocalStaticPrefixHarness {
    static void require(boolean ok){if(!ok)throw new AssertionError("owned local static prefix");}
    public static void main(String[]args)throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int mask=route==3?14:(mode%8)<<1,scenario=mode/8;
        Function1<Object,Object> previousFirst=null,previousSecond=null;
        for(int attempt=0;attempt<2;attempt++) {
            Trace.events=0;Trace.tags=0;Trace.partial=null;Trace.firstSeen=null;Trace.secondSeen=null;Trace.seen=null;
            Trace.result=scenario==1?null:new Object();
            Trace.fail=scenario==2?3:scenario==3?(mode%2==0?5:6):0;
            Trace.failure=mode%2==0?new IllegalStateException("owned runtime"):new AssertionError("owned error");
            Function1<Object,Object> first=new LocalStaticPrefixProbe(8),second=new LocalStaticPrefixProbe(9);
            int count=Integer.MIN_VALUE+mode,capacity=Integer.MAX_VALUE-attempt;
            Core<?> value=null;Throwable failure=null;Object a=null,b=null;
            Object token=scenario==1?null:new Object[]{new Object(),null};
            try {
                value=LocalStaticPrefixCaller.make(route,count,capacity,first,second,mask);
                a=value.first.invoke(token);b=value.second.invoke(token);
            }catch(Throwable error){failure=error;}
            require(Trace.partial instanceof Core);
            Core<?> partial=(Core<?>)Trace.partial;
            require(partial.count==count&&partial.capacity==((mask&2)!=0?7:capacity));
            require(((mask&4)==0?partial.first==first:partial.first!=first)&&((mask&8)==0?partial.second==second:partial.second!=second));
            if(attempt==1){if((mask&4)!=0)require(partial.first==previousFirst);if((mask&8)!=0)require(partial.second==previousSecond);}
            previousFirst=partial.first;previousSecond=partial.second;
            if(Trace.fail==3)require(value==null&&Trace.firstSeen==null&&Trace.secondSeen==null&&Trace.events==3&&Trace.tags==0&&Trace.seen==null);
            else {
                require(value==partial&&Trace.firstSeen==partial.first&&Trace.secondSeen==partial.second);
                int one=(mask&4)!=0?1:8,two=(mask&8)!=0?2:9;
                require(Trace.events==(Trace.fail==5?345:Trace.fail==6?3456:345656));
                require(Trace.tags==(Trace.fail==0?one*10+two:one)&&Trace.seen==(Trace.fail==5?null:token));
                if(Trace.fail==0)require(a==Trace.result&&b==Trace.result);
            }
            require(failure==(Trace.fail==0?null:Trace.failure));
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",events="+Trace.events);
        }
    }
}
