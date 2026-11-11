import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class InterleavedAllocationHarness {
    static final boolean NESTED=false;
    static void require(boolean ok,String what){if(!ok)throw new AssertionError("owned interleaved allocation: "+what);}
    static Object child(Object parent){try{return parent.getClass().getField("child").get(parent);}catch(ReflectiveOperationException failure){throw new AssertionError(failure);}}
    static Target call(Provider provider,long bits,int route) throws Throwable {
        if(route==0)return Core.make(provider,bits);
        if(route==1)return InterleavedAllocationCaller.call(provider,bits);
        if(route==2){try{return (Target)Core.class.getMethod("make",Provider.class,long.class).invoke(null,provider,bits);}catch(InvocationTargetException p){throw p.getCause();}}
        return (Target)MethodHandles.publicLookup().findStatic(Core.class,"make",MethodType.methodType(Target.class,Provider.class,long.class)).invoke(provider,bits);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        int fail=mode>=24?8+(mode%2):mode>=4&&mode<=9?mode-3:mode>=10&&mode<=15?mode-9:0;
        boolean fatal=mode>=10&&mode<=15||mode==24||mode==25,dead=mode==16;
        Provider p=new Provider();Trace.provider=p;Trace.failAt=fail;
        RuntimeException failure=new RuntimeException("owned staged failure");Error error=new Error("owned staged Error");
        Trace.failure=failure;Trace.fatal=fatal?error:null;
        for(int attempt=0;attempt<2;attempt++) {
            int branch=mode==2||mode==23?0:mode==1?1:mode>=4&&mode<=15?(mode%2==0?1:2):2;
            int originalFlag=mode%2;Object first=mode==17?null:new Object(),alternate=mode==22?null:new Object();
            Object initFirst=mode==17?null:new Object(),initAlternate=mode==22?null:new Object();
            p.branch=branch;p.flag=originalFlag;p.first=first;p.alternate=alternate;
            Trace.initFirst=initFirst;Trace.initAlternate=initAlternate;Trace.initFlag=1-originalFlag;
            Trace.events=0;Trace.calls=0;Trace.target=null;Trace.outer=null;Trace.input=71;Trace.returned=-1;
            long input=mode==18?Long.MIN_VALUE:mode==19?Long.MAX_VALUE:0x7fedcba987654321L+attempt;
            long returned=input^0x102030405060708L;
            boolean allocated=!dead&&branch!=0,sticky=allocated&&fail==1&&attempt==1;
            Object selected=branch==1?(attempt==0?initAlternate:alternate):
                (attempt==0?Trace.initFlag:originalFlag)==0?(attempt==0?initFirst:first):(attempt==0?initAlternate:alternate);
            Trace.observer=stage->{
                if(stage==1)require(p.first==first&&p.alternate==alternate&&p.flag==originalFlag,"read moved before allocation");
                if(stage==2){require(Trace.target==null,"tail moved across construction");p.first=new Object();p.alternate=new Object();p.flag=99;Trace.returned=returned+1;}
                if(stage==3){require(Trace.input==input,"opaque input changed");Trace.returned=returned;}
                if(stage==4){require(Trace.target!=null&&Trace.target.value==null&&Trace.target.bits==0,"partial defaults changed");Trace.returned=~returned;}
                if(stage==5)require(Trace.target.value==selected&&Trace.target.bits==0,"selected reference/store timing changed");
                if(stage==6)require(Trace.target.value==selected&&Trace.target.bits==returned,"captured wide result changed");
                if(stage==7)require(Trace.target==null&&Trace.outer==null&&Trace.calls==0,"outer initialization moved");
                if(stage==8)require(Trace.outer!=null&&child(Trace.outer)==null&&Trace.target!=null&&Trace.target.value==selected,"outer partial defaults changed");
                if(stage==9)require(child(Trace.outer)==Trace.target,"outer final store timing changed");
            };
            Target result=null;Throwable caught=null;
            try{result=call(dead?null:p,input,route);}catch(Throwable problem){caught=problem;}
            long wanted=0;
            if(allocated&&!sticky){if(attempt==0)wanted=NESTED?0x71:1;if(fail!=1)for(int stage=2;stage<=6;stage++){wanted=wanted*16+stage;if(stage==fail)break;}if(NESTED&&(fail==0||fail>=8))for(int stage=8;stage<=9;stage++){wanted=wanted*16+stage;if(stage==fail)break;}}
            require(Trace.events==wanted,"event order changed");
            boolean tail=allocated&&!sticky&&fail!=1,started=tail&&fail!=2&&fail!=3;
            require(Trace.calls==(tail?1:0)&&(Trace.target!=null)==started,"call count/publication changed");
            require(Trace.input==(tail&&fail!=2?input:71),"partial opaque effect changed");
            if(started){require(Trace.target.value==(fail==4?null:selected),"partial reference changed");require(Trace.target.bits==(fail==4||fail==5?0:returned),"partial wide value changed");}
            boolean outerStarted=NESTED&&started&&fail!=4&&fail!=5&&fail!=6;
            require((Trace.outer!=null)==outerStarted,"outer publication changed");
            if(outerStarted)require(child(Trace.outer)==(fail==8?null:Trace.target),"partial outer final field changed");
            if(dead)require(caught instanceof NullPointerException,"null provider boundary changed");
            else if(branch==0)require(caught==null&&result==null,"unallocated switch branch changed");
            else if(fail==1){if(attempt==1)require(caught instanceof NoClassDefFoundError,"initialization failure not sticky");else if(fatal)require(caught==error,"initializer Error changed");else require(caught instanceof ExceptionInInitializerError&&caught.getCause()==failure,"initializer failure changed");}
            else if(fail!=0)require(caught==(fatal?error:failure),"throwable identity changed");
            else require(caught==null&&result==Trace.target&&result.value==selected&&result.bits==returned,"constructed result changed");
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
