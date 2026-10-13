# Tools\LocalSettings.example.psd1 —— 本机设置的模板。
#
# 复制成 Tools\LocalSettings.psd1（那个文件被 .gitignore 忽略，不会进仓库），按需填写。
# 一个键都不用填也行：宿主工程、引擎目录在插件放在 <宿主>\Plugins\DreamTextureForUnreal\ 、
# 引擎是 Launcher 版时都能自动找到。
#
# Tools\ 下脚本找一个值的顺序（先找到的算数）：
#   1. 脚本自己的参数；
#   2. 环境变量（见下面各键的说明）；
#   3. 本文件的键；
#   4. 能自动推出来的就自动推（宿主工程、引擎目录）；
#   5. 都没有就失败，并告诉你该设哪一个。
# 看每个值最后是从哪一层来的：pwsh -NoProfile -File Tools\Show-Settings.ps1
#
# 路径用单引号括起来，反斜杠不用转义。不需要的键整行删掉或注释掉，不要留空串。
@{
	# 引擎目录：里面有 Engine\ 的那一层（Launcher 版是 ...\UE_5.8，源码构建是仓库根）。
	# 环境变量 DREAMTEXTURE_ENGINE_ROOT。不填就按宿主 .uproject 的 EngineAssociation 查：
	# Launcher 版查 HKLM:\SOFTWARE\EpicGames\Unreal Engine\<版本> 的 InstalledDirectory，
	# 源码构建（GUID）查 HKCU:\Software\Epic Games\Unreal Engine\Builds，再退到 LauncherInstalled.dat。
	# EngineRoot = 'C:\Program Files\Epic Games\UE_5.8'

	# 宿主工程的 .uproject（Test.ps1 的 -Project）。不填就找插件往上两级目录里的那个 .uproject。
	# HostProject = 'D:\UE\DreamTextureTestHost\DreamTextureTestHost.uproject'

	# 宿主工程目录（New-DreamTextureTestHost.ps1 的 -Root）。不填就取 HostProject 所在目录，
	# 再不行就取插件往上两级的宿主；都没有时创建宿主必须给它。
	# HostRoot = 'D:\UE\DreamTextureTestHost'

	# 插件仓库的某个工作区（New-DreamTextureTestHost.ps1 的 -RepoPath），宿主的 worktree 从它加出来。
	# 不填就用脚本自己所在的 git 仓库。
	# PluginRepo = 'D:\Code\DreamTextureForUnreal'

	# 固定数据后端（Tools\FixtureBackend\，Test.ps1 -Live）用的 dreamtexture.exe。环境变量 DREAMTEXTURE_BACKEND_EXE
	# （优先于本键，两个后端共用）。工作流目录 workflows\ 随它所在的目录走。
	# BackendExe = 'D:\DreamTexture\dreamtexture.exe'

	# 流水线后端（Tools\PipelineBackend\，Test.ps1 -Pipeline）用的 dreamtexture.exe，通常是带着插件所需新接口的那份
	# （后端有改动、自己编了新版时）。环境变量 DREAMTEXTURE_BACKEND_EXE 优先；不填就退到 BackendExe。
	# 自研节点包 comfy_nodes\ 也随它所在的目录走。
	# PipelineBackendExe = 'D:\DreamTexture-dev\dreamtexture.exe'

	# 流水线后端借用的 ComfyUI 整合包根目录（里面有 python\ 和 ComfyUI\）。环境变量 DREAMTEXTURE_COMFY_ROOT。
	# 只读用它的 Python 和节点包，另起一份隔离的 ComfyUI，不碰这份的数据。
	# ComfyRoot = 'D:\Tools\ComfyUI-portable'
}
