package fixture;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.MethodType;

public class SDKCovariantInheritanceHarness {
    static class Fault extends RuntimeException {}
    static class Stop extends Error {}
    static void bad() {throw new AssertionError("owned SDK covariance mismatch");}
    static void attempt(int route,final int mode) throws Throwable {
        Trace.hook=null;
        final Core<Object,Object> core=new Core<Object,Object>();
        final Object first=new Object(), other=new Object(), tail=new Object();
        final Slice<Object,Object> primary=new Slice<Object,Object>(first,tail);
        final Slice<Object,Object> alternate=new Slice<Object,Object>(other,tail);
        if(mode==14)alternate.first=first;
        core.result=mode==1?null:primary;
        final Failure failure=new Failure();
        final Fault fault=new Fault();
        final Stop stop=new Stop();
        final int thrownAt=mode==5||mode==8?1:mode==6||mode==9?2:mode==7||mode==10?3:0;
        final Throwable expectedThrow=mode>=5&&mode<=7?failure:mode==8||mode==10?fault:mode==9?stop:null;
        final Slice<Object,Object> expectedResult=mode==1?null:mode==2?alternate:primary;
        final Object expectedFirst=mode==1?null:mode==2||mode==4||mode==11||mode==12?other:first;
        final int[] ticks={0};
        Trace.stage=Trace.count=Trace.events=0;
        Trace.hook=mode==15?null:new Hook() {
            public void tick() throws Failure {
                ticks[0]++;
                int phase=Trace.stage;
                if(ticks[0]!=Trace.count)bad();
                if(phase!=Trace.count||phase<1||phase>3)bad();
                if(mode==2&&phase==1||mode==3&&phase==2||mode==13&&phase==3)core.result=alternate;
                if(mode==4&&phase==3||mode==11&&phase==1||mode==12&&phase==2)primary.first=other;
                if(mode==12&&phase==2)core.result=alternate;
                if(phase==thrownAt) {
                    if(expectedThrow==failure)throw failure;
                    if(expectedThrow==fault)throw fault;
                    throw stop;
                }
            }
        };
        try {
            View<Object> value;
            if(route==0)value=core.view();
            else if(route==1)value=((Catalog<Object>)core).view();
            else {
                MethodHandle call=MethodHandles.lookup().findVirtual(Core.class,"view",MethodType.methodType(Slice.class));
                value=(Slice<Object,Object>)call.invokeExact(core);
            }
            if(value!=expectedResult)bad();
            Object result=value==null?null:value.head();
            if(result!=expectedFirst||expectedThrow!=null)bad();
        } catch(Throwable caught) {if(caught!=expectedThrow)bad();}
        int phases=thrownAt!=0?thrownAt:mode==1?2:3;
        if(ticks[0]!=(mode==15?0:phases))bad();
        if(Trace.count!=phases||Trace.stage!=phases||Trace.events!=(phases==1?1:phases==2?12:123))bad();
        if(core.result!=(mode==1?null:mode==2||mode==3||mode==12||mode==13?alternate:primary))bad();
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        attempt(route,mode);attempt(route,mode);
        System.out.println("attempts=2,sdk-covariant-inheritance=matched");
    }
}
