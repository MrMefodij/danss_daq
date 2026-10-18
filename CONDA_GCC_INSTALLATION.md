# Installation guide

```bash
cat /etc/os-release
mkdir $HOME/daq
mkdir $HOME/daq/packages

git clone https://gitlab.triumf.ca/midas/midas.git
git submodule sync
git submodule update --init --recursive --remote
```

Правим и добавляем в конец ' $HOME/.bashrc '

```bash
export MIDASSYS=$HOME/daq/packages/midas
export MIDAS_EXPTAB=$HOME/daq/online/exptab
export MIDAS_EXPT_NAME=danss
export PATH=$PATH:$MIDASSYS/bin
export PATH="$HOME/.local/bin:$PATH"
alias conda=$HOME/miniforge3/bin/conda

if [ -d "$HOME/miniforge3/envs/daq" ]; then
  export ROOTSYS=$HOME/miniforge3/envs/daq
  export PATH=$PATH:$ROOTSYS/bin    
  export ROOT_INCLUDE_DIRS=$($ROOTSYS/bin/root-config --incdir)
  export ROOT_LIBRARIES="$($ROOTSYS/bin/root-config --libs)"
fi
```
Возвращаемся в командную строку и продолжаем установку:

```bash
wget https://github.com/conda-forge/miniforge/releases/latest/download/Miniforge3-Linux-x86_64.sh
bash Miniforge3-Linux-x86_64.sh -b -p $HOME/miniforge3
$HOME/miniforge3/bin/conda create -n gcc13 -c conda-forge gxx_linux-64=13 gcc_linux-64=13 -y

export CONDA_PKGS_DIRS=$HOME/.conda/pkgs

~/miniforge3/bin/conda create -p $HOME/miniforge3/envs/daq -c conda-forge \
    root=6.40.04 root_cxx_standard=20 gxx_linux-64 gcc_linux-64 zlib -y
    
~/miniforge3/bin/conda install -p $HOME/miniforge3/envs/daq -c conda-forge zlib -y

```