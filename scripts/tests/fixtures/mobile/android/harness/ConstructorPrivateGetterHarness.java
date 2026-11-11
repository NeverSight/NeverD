import fixture.*;
import java.lang.invoke.*;
import java.lang.reflect.*;
public final class ConstructorPrivateGetterHarness {
    static void require(boolean ok) { if(!ok)throw new AssertionError("owned constructor private getter"); }
    static Core create(int route,boolean explicit,int mask,Core fallback,Object token) throws Throwable {
        if(route==0)return new Core(explicit,mask,fallback,token);
        if(route==1)return ConstructorPrivateGetterCaller.create(explicit,mask,fallback,token);
        if(route==2)try{return Core.class.getConstructor(boolean.class,int.class,Core.class,Object.class).newInstance(explicit,mask,fallback,token);}catch(InvocationTargetException e){throw e.getCause();}
        return (Core)MethodHandles.publicLookup().findConstructor(Core.class,MethodType.methodType(void.class,boolean.class,int.class,Core.class,Object.class)).invokeExact(explicit,mask,fallback,token);
    }
    public static void main(String[] args) throws Throwable {
        int route=Integer.parseInt(args[0]),mode=Integer.parseInt(args[1]);
        Trace.failAt=0;Trace.observer=null;
        Core first=new Core(false,null,null),replacement=new Core(true,null,null);
        Trace.peer=mode==2?null:first;Trace.replacement=replacement;
        RuntimeException failure=new IllegalStateException("owned private getter failure");Trace.failure=failure;
        Core nextDefault=Trace.peer,previous=null;
        for(int attempt=0;attempt<2;attempt++) {
            boolean explicit=mode!=9;int mask=mode==1||mode==11?0:mode==10?2:1;
            Core fallback=mode==11?null:first,selected=(mask&1)!=0?nextDefault:fallback;
            boolean expectedValue=(mask&1)!=0&&selected!=null?selected.value:explicit;
            Object token=mode==9?null:new Object();Trace.events=0;Trace.seen=null;Trace.partial=null;
            Trace.failAt=mode==3?1:mode==4?2:mode==5?3:mode==6?4:mode==7?5:mode==8?7:0;
            int[] observations={0};
            Trace.observer=event->{
                observations[0]++;
                if(event==1)require(Trace.seen==null&&Trace.partial==null);
                if(event>=2&&event<=4)require(Trace.seen==selected&&Trace.partial==null);
                if(event==5)require(Trace.partial!=null && !Trace.partial.value && Trace.partial.peer==null && Trace.partial.token==null);
                if(event==7)require(Trace.partial!=null&&Trace.partial.value==expectedValue&&Trace.partial.peer==selected&&Trace.partial.token==token);
            };
            Core result=null;Throwable caught=null;try{result=create(route,explicit,mask,fallback,token);}catch(Throwable e){caught=e;}
            boolean uses=(mask&1)!=0;String sequence=uses?(attempt==0?"1":"")+(selected==null?"":"23457"):"57";
            if(mode==3&&attempt==1)sequence="";
            if(Trace.failAt!=0&&sequence.indexOf((char)('0'+Trace.failAt))>=0)sequence=sequence.substring(0,sequence.indexOf((char)('0'+Trace.failAt))+1);
            require(Trace.events==(sequence.isEmpty()?0:Long.parseLong(sequence))&&observations[0]==sequence.length());
            if(mode==3)require(attempt==0?caught instanceof ExceptionInInitializerError&&caught.getCause()==failure:caught instanceof NoClassDefFoundError);
            else if(mode==2)require(caught instanceof NullPointerException);
            else if(mode>=4&&mode<=8)require(caught==failure);
            else {require(caught==null&&result!=null&&result==Trace.partial&&result!=previous&&result.value==expectedValue&&result.peer==selected&&result.token==token);previous=result;}
            boolean getter=uses&&selected!=null&&mode!=3;
            require(Trace.seen==(getter?selected:null));
            boolean primary=!uses||getter&&mode!=4&&mode!=5&&mode!=6;
            require((Trace.partial!=null)==primary);
            if(primary)require(mode==7?!Trace.partial.value&&Trace.partial.peer==null&&Trace.partial.token==null:Trace.partial.value==expectedValue&&Trace.partial.peer==selected&&Trace.partial.token==token);
            if(uses&&mode!=3) {
                if(getter&&mode!=4&&mode!=5)nextDefault=replacement;
                require(Defaults.peer==nextDefault);
            }
            System.out.println("route="+route+",mode="+mode+",attempt="+attempt+",trace="+Trace.events);
        }
    }
}
